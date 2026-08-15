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
    // Each adapter answers nullptr when it does not apply -- the platform plugin is not its own, or
    // the OS is too old -- and the null object is constructed once, here. Same shape as
    // makeWindowShadow next door, rather than a second convention in the same directory.
#ifdef _WIN32
    if (auto frame = makeWin32WindowFrame(colorScheme))
        return frame;
#elifdef __APPLE__
    if (auto frame = makeCocoaWindowFrame())
        return frame;
#endif

    // The Linux and BSD desktops land here by construction -- there is no OS frame service to keep,
    // so the window is genuinely frameless and its shadow comes from the compositor instead
    // (@see makeWindowShadow) -- and so does a Windows or macOS process running under a platform
    // plugin that is not its own, where touching the native handle would be undefined.
    return std::make_unique<NullWindowFrame>();
}

} // namespace contour::platform
