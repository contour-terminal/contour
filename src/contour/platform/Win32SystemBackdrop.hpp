// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/SystemBackdrop.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#ifdef _WIN32

    #include <contour/platform/Dwmapi.hpp>

    #include <expected>

class QWindow;

namespace contour::platform
{

/// @file
/// The one writer of a window's DWMWA_SYSTEMBACKDROP_TYPE and of its DwmExtendFrameIntoClientArea margins.
///
/// Three parties want a say in them: the title-band material (Mica Alt behind the band), blur-behind
/// (`background.blur`, Acrylic over the whole window) and the kept frame (the margins that keep DWM's shadow
/// on, and their re-extension after a composition restart). Each records its wish on the QWindow and calls
/// @ref applySystemBackdrop, which decides from all of them (@ref systemBackdropFor, @ref frameMarginsFor),
/// so no party overwrites another's (R-h). The records live on the window, not in a table keyed by handle,
/// for the reason @ref needsFrameApplied gives.

/// Records what the title band shows behind it, and which window asks.
/// @pre GUI thread.
/// @param window The window.
/// @param band The band's backdrop.
/// @param kind A tabbed terminal window or Settings.
void recordBandBackdrop(QWindow& window, config::BandBackdrop band, config::BackdropKind kind);

/// Records whether the user asked for blur behind the window.
/// @pre GUI thread.
/// @param window The window.
/// @param blur The wish.
void recordBlurBehind(QWindow& window, BlurBehindWish blur);

/// @pre GUI thread.
/// @param window The window.
/// @return The blur wish recorded on @p window; @c Off when none was.
[[nodiscard]] BlurBehindWish recordedBlurBehind(QWindow const& window);

/// Writes the backdrop and the margins @p window's records decide. A window with no record of a kind takes
/// its default: the band of the client's own paint, a tabbed window, no blur, the kept client frame.
/// @pre GUI thread.
/// @param window The window whose records decide.
/// @param hwnd Its native handle.
/// @param api The DWM entry points; injected, so a test passes a stub.
/// @return Nothing on success. When DWM rejects the backdrop it is set back to none, the margins to those of
///         a window without one, and the result is @c Unsupported when DWM does not know the attribute
///         (E_INVALIDARG, before Windows 11 22H2) or @c Rejected otherwise. Where the decided backdrop is
///         none, DWM's not knowing the attribute is no failure: nothing was asked of it.
[[nodiscard]] std::expected<void, TitleBandMaterialError> applySystemBackdrop(QWindow& window,
                                                                              HWND hwnd,
                                                                              DwmApi const& api);

} // namespace contour::platform

#endif
