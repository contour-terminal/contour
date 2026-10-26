// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TitleBandMaterial.hpp>

namespace contour::platform
{

#ifndef __APPLE__

std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial()
{
    return std::make_unique<NullTitleBandMaterial>();
}

#else

// Temporary: the null material on macOS too, until CocoaTitleBandMaterial.mm defines the native one.
std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial()
{
    return std::make_unique<NullTitleBandMaterial>();
}

#endif

} // namespace contour::platform
