// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QSurfaceFormat>

namespace contour::display
{

/// Whether Qt resolved OpenGL ES (rather than desktop OpenGL) for this process.
[[nodiscard]] bool useOpenGLES() noexcept;

/// The default surface format for the scene graph's OpenGL context (the RHI backend runs on
/// OpenGL, see ContourGuiApp: QQuickWindow::setGraphicsApi(OpenGL)).
[[nodiscard]] QSurfaceFormat createSurfaceFormat();

} // namespace contour::display
