// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NativeCaptionButtons.hpp>

#ifdef _WIN32

namespace contour::platform
{

/// The Win32 caption adapter: answers WM_NCHITTEST with HTMAXBUTTON over an attached window's maximise
/// button, which is what makes Windows 11 offer Snap Layouts there, and drives that button through the
/// non-client mouse messages that follow (platform::onNcEvent).
///
/// Returned unconditionally, because it is built before a QGuiApplication exists: every call checks that
/// the platform plugin is "windows" and does nothing otherwise, so the offscreen and minimal plugins on a
/// Windows build never see a native handle touched.
/// @return The adapter; never null.
[[nodiscard]] std::unique_ptr<INativeCaptionButtons> makeWin32CaptionButtons();

} // namespace contour::platform

#endif
