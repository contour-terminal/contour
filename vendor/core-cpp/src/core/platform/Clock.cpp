// SPDX-License-Identifier: Apache-2.0

#include <core/platform/Clock.hpp>

namespace core::platform
{

IWallClock& defaultSystemWallClock() noexcept
{
    static SystemWallClock instance;
    return instance;
}

} // namespace core::platform
