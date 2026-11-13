// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/SystemBackdrop.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <QtGui/QWindow>

#include <expected>

namespace contour::platform
{

/// Asks the compositor to blur what lies behind @p window, or to stop.
/// @param window The window.
/// @param blur Whether to blur behind it.
/// @param region The region to blur; empty is the whole window.
void setBlurBehind(QWindow* window, BlurBehindWish blur, QRegion const& region = QRegion());

/// Asks the compositor to blur behind @p window's title band -- its top @p bandHeight logical pixels -- or,
/// with 0, to stop. Combined with setBlurBehind()'s wish for the same window: a window blurred whole stays
/// blurred whole.
/// @param window     The window.
/// @param bandHeight The band's height in logical pixels; 0 asks for no blur behind it.
/// @return Nothing where the request reached a compositor that blurs (KWin on Wayland; any X11 window
///         manager, which ignores what it does not know); TitleBandMaterialError::Unsupported where none is
///         reachable: a Wayland compositor without org_kde_kwin_blur, or another platform.
[[nodiscard]] std::expected<void, TitleBandMaterialError> setTitleBandBlur(QWindow& window, qreal bandHeight);

} // namespace contour::platform
