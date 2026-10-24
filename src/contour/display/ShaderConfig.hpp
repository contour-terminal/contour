// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QSurfaceFormat>

#include <cstdint>

namespace contour::display
{

/// Whether Qt resolved OpenGL ES (rather than desktop OpenGL) for this process.
[[nodiscard]] bool useOpenGLES() noexcept;

/// Whether a frame waits for the display's vertical blank before it is swapped in.
enum class SwapPacing : uint8_t
{
    VSync = 0,  ///< Swap on the vertical blank: the normal, tear-free pacing.
    Unpaced = 1 ///< Swap at once, which a latency measurement needs: vsync quantizes every sample.
};

/// The default surface format for the scene graph's OpenGL context (the RHI backend runs on
/// OpenGL, see ContourGuiApp: QQuickWindow::setGraphicsApi(OpenGL)).
/// @param pacing Whether swaps wait for the vertical blank (swap interval 1) or not (0).
[[nodiscard]] QSurfaceFormat createSurfaceFormat(SwapPacing pacing);

} // namespace contour::display
