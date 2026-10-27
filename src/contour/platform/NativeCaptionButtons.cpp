// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeCaptionButtons.hpp>

#ifdef _WIN32
    #include <contour/platform/Win32CaptionButtons.hpp>
#endif

namespace contour::platform
{

std::unique_ptr<INativeCaptionButtons> makeNativeCaptionButtons()
{
    // Built before the application exists, so the Win32 adapter is returned on every Windows build and
    // checks the platform plugin on each call; elsewhere there is no caption button to hand over.
#ifdef _WIN32
    return makeWin32CaptionButtons();
#else
    return std::make_unique<NullNativeCaptionButtons>();
#endif
}

} // namespace contour::platform
