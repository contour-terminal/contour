// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeMenuPresenter.hpp>

// macOS has its own presenter, CocoaNativeMenuPresenter.mm.
#ifndef __APPLE__

namespace contour::platform
{

std::unique_ptr<INativeMenuPresenter> makeNativeMenuPresenter()
{
    return std::make_unique<NullNativeMenuPresenter>();
}

} // namespace contour::platform

#endif
