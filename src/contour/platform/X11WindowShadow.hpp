// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/WindowShadow.hpp>
#include <contour/platform/XcbProperty.hpp>

#ifdef CONTOUR_FRONTEND_XCB

namespace contour::platform
{

/// The X11 shadow attachment for @p window, or a NullWindowShadow.
///
/// Null when the platform is not xcb, when the server's image byte order is not the little-endian
/// one `QImage::Format_ARGB32` lays out, or when depth-32 pixmaps are unavailable -- each of which
/// would otherwise mean uploading garbage rather than a shadow.
[[nodiscard]] std::unique_ptr<WindowShadow> makeX11WindowShadow(QWindow& window);

} // namespace contour::platform

#endif
