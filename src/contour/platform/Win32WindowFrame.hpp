// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NativeWindowFrame.hpp>

#ifdef _WIN32

namespace contour::platform
{

/// The Win32 frame adapter.
///
/// Keeps `WS_THICKFRAME | WS_CAPTION` on the window and reports a zero-thickness frame from
/// `WM_NCCALCSIZE`, so the client area covers the whole window -- our tab strip still draws
/// everything -- while DWM still sees a framed window and gives it a drop shadow and rounded
/// corners. That is what `Qt::FramelessWindowHint` threw away.
///
/// @param colorScheme Supplies the effective dark/light scheme for the border DWM draws around the
///                    rounded corners.
[[nodiscard]] std::unique_ptr<NativeWindowFrame> makeWin32WindowFrame(
    std::function<Qt::ColorScheme()> colorScheme);

} // namespace contour::platform

#endif
