// SPDX-License-Identifier: Apache-2.0
//
// Where a user's caches live below the home directory: the XDG Base Directory default, on Linux, the BSDs and
// every POSIX system but macOS.
#include <core/platform/UserPaths.hpp>

#include <string_view>

namespace core::platform::detail
{

std::string_view cacheHomeBelowHome() noexcept
{
    return ".cache";
}

} // namespace core::platform::detail
