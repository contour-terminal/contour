// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeWindowFrame.hpp>

#ifdef _WIN32
    #include <contour/platform/Win32WindowFrame.hpp>
#endif

#ifdef __APPLE__
    #include <contour/platform/CocoaWindowFrame.hpp>
#endif

#include <utility>

namespace contour::platform
{

std::unique_ptr<NativeWindowFrame> makeNativeWindowFrame(
    [[maybe_unused]] std::function<Qt::ColorScheme()> const& colorScheme)
{
#ifdef _WIN32
    return makeWin32WindowFrame(colorScheme);
#elifdef __APPLE__
    return makeCocoaWindowFrame();
#else
    // The Linux and BSD desktops: there is no OS frame service to keep here, so the window is
    // genuinely frameless and its shadow comes from the compositor instead. @see makeWindowShadow.
    return std::make_unique<NullWindowFrame>();
#endif
}

} // namespace contour::platform
