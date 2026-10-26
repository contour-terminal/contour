// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NativeWindowFrame.hpp>

#ifdef __APPLE__

namespace contour::platform
{

/// The Cocoa frame adapter.
///
/// Keeps the window a decorated NSWindow whose content view has the whole of it, rather than making it
/// borderless. AppKit then keeps drawing the drop shadow, the rounded corners, the traffic lights, the
/// full-screen button, window tabbing, edge resizing and double-click-to-zoom -- all of which
/// `Qt::FramelessWindowHint` gave up, and all of which would otherwise have to be reimplemented badly.
///
/// The full-size content view itself (`NSWindowStyleMaskFullSizeContentView`) is Qt's
/// `Qt::ExpandedClientAreaHint`, which also reports the title bar as the window's safe area; this
/// adapter leaves the style mask alone and only hides the title and keeps the shadow (Step 1d spike,
/// D1 = Hint: one owner of the safe area). Nor does it touch the title bar's background: the controller
/// asks Qt to clear it (`Qt::NoTitleBarBackgroundHint`, set through Main.qml's `flags` while the band's
/// backdrop is the client's own, which is the shipped band, ruling S6), and the title bar's appearance and
/// toolbar are CocoaTitleBandMaterial's.
[[nodiscard]] std::unique_ptr<NativeWindowFrame> makeCocoaWindowFrame();

} // namespace contour::platform

#endif
