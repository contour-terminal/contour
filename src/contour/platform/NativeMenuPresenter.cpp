// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeMenuPresenter.hpp>

namespace contour::platform
{

std::unique_ptr<INativeMenuPresenter> makeNativeMenuPresenter()
{
    return std::make_unique<NullNativeMenuPresenter>();
}

} // namespace contour::platform
