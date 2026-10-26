// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TitleBandMaterial.hpp>

// macOS defines makeTitleBandMaterial() in CocoaTitleBandMaterial.mm.
#ifndef __APPLE__

namespace contour::platform
{

std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial()
{
    return std::make_unique<NullTitleBandMaterial>();
}

} // namespace contour::platform

#endif
