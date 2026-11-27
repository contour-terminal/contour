// SPDX-License-Identifier: Apache-2.0
//
// Where a user's caches live below the home directory: where %LOCALAPPDATA% points on every Windows since
// Vista.
#include <core/platform/UserPaths.hpp>

#include <string_view>

namespace core::platform::detail
{

std::string_view cacheHomeBelowHome() noexcept
{
    return "AppData/Local";
}

} // namespace core::platform::detail
