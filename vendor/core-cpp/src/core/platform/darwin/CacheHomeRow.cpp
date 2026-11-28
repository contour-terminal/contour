// SPDX-License-Identifier: Apache-2.0
//
// Where a user's caches live below the home directory: macOS's per-user caches.
#include <core/platform/UserPaths.hpp>

#include <string_view>

namespace core::platform::detail
{

std::string_view cacheHomeBelowHome() noexcept
{
    return "Library/Caches";
}

} // namespace core::platform::detail
