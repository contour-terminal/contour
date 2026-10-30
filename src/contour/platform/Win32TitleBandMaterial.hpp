// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/TitleBandMaterial.hpp>

#ifdef _WIN32

    #include <contour/platform/Dwmapi.hpp>

    #include <memory>

namespace contour::platform
{

/// The Windows title-band material: Mica Alt behind a tabbed window's band, Mica behind Settings.
///
/// It owns the DWM backdrop and the frame margins, through @ref applySystemBackdrop; the kept frame
/// (Win32WindowFrame) owns the style bits, WM_NCCALCSIZE, the dark-mode attribute and the corner preference
/// (R-h). Blur-behind writes the same attribute through the same function, and wins while it is on (R-i).
///
/// The band shows the backdrop only where the scene leaves it transparent, so the window's swapchain must
/// compose with premultiplied alpha: Qt's D3D11 backend does for a QQuickWindow whose format has an alpha
/// channel (display/ShaderConfig.cpp) and whose colour is transparent (Main.qml), through DirectComposition
/// since Qt 6.4. An opaque swapchain shows a black band and raises no error, which no test here can see
/// (owner check O-3).
///
/// Built before the QGuiApplication exists, so it checks the platform plugin on every call (F1): off the
/// native Windows platform apply() does nothing and succeeds.
///
/// @param api The DWM entry points, held by reference; injected, so a test passes a stub.
/// @return The material, or nullptr when @p api lacks the backdrop's two entry points.
[[nodiscard]] std::unique_ptr<ITitleBandMaterial> makeWin32TitleBandMaterial(DwmApi const& api);

} // namespace contour::platform

#endif
