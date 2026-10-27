// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeCaptionButtons.hpp>

namespace contour::platform
{

std::unique_ptr<INativeCaptionButtons> makeNativeCaptionButtons()
{
    return std::make_unique<NullNativeCaptionButtons>();
}

} // namespace contour::platform
