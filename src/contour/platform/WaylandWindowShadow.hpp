// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/WindowShadow.hpp>

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)

namespace contour::platform
{

/// The Wayland shadow attachment for @p window, or a NullWindowShadow.
///
/// Null when the platform is not Wayland or when the compositor does not offer
/// `org_kde_kwin_shadow` -- which is every compositor but KWin.
[[nodiscard]] std::unique_ptr<WindowShadow> makeWaylandWindowShadow(QWindow& window);

} // namespace contour::platform

#endif
