// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TitleBandMaterial.hpp>

#ifdef _WIN32
    #include <contour/platform/Win32TitleBandMaterial.hpp>
#else
    #include <contour/platform/KWinTitleBandMaterial.hpp>
#endif

// macOS defines makeTitleBandMaterial() in CocoaTitleBandMaterial.mm.
#ifndef __APPLE__

namespace contour::platform
{

std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial()
{
    #ifdef _WIN32
    if (auto material = makeWin32TitleBandMaterial(dwmApi()))
        return material;
    return std::make_unique<NullTitleBandMaterial>();
    #else
    // The Linux and BSD desktops: KWin blurs behind the band, and every other compositor reports it
    // unsupported when a blurred band is asked for.
    return makeKWinTitleBandMaterial();
    #endif
}

} // namespace contour::platform

#endif
