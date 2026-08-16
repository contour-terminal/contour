// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NativeWindowFrame.hpp>

#ifdef __APPLE__

namespace contour::platform
{

/// The Cocoa frame adapter.
///
/// Keeps the window a decorated NSWindow and gives its content view the whole of it
/// (`NSWindowStyleMaskFullSizeContentView`, with the title bar transparent and its title hidden),
/// rather than making it borderless. AppKit then keeps drawing the drop shadow, the rounded
/// corners, the traffic lights, the full-screen button, window tabbing, edge resizing and
/// double-click-to-zoom -- all of which `Qt::FramelessWindowHint` gave up, and all of which would
/// otherwise have to be reimplemented badly.
[[nodiscard]] std::unique_ptr<NativeWindowFrame> makeCocoaWindowFrame();

} // namespace contour::platform

#endif
