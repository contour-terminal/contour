// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/SystemBackdrop.hpp>

#include <QtGui/QWindow>

namespace contour::platform
{

/// Asks the compositor to blur what lies behind @p window, or to stop.
/// @param window The window.
/// @param blur Whether to blur behind it.
/// @param region The region to blur; empty is the whole window.
void setBlurBehind(QWindow* window, BlurBehindWish blur, QRegion const& region = QRegion());

} // namespace contour::platform
