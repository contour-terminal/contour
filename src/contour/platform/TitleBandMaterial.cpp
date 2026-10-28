// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TitleBandMaterial.hpp>

#ifdef _WIN32
    #include <contour/platform/Win32TitleBandMaterial.hpp>
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
    #endif
    return std::make_unique<NullTitleBandMaterial>();
}

} // namespace contour::platform

#endif
