// SPDX-License-Identifier: Apache-2.0
//
// cacheHome's rows on Windows: %LOCALAPPDATA%, and AppData/Local below the profile where it is unset.
#include <core/platform/UserPathsTestSupport.hpp>

#include <vector>

namespace core::platform::test
{

std::vector<CacheHomeCase> cacheHomeCases()
{
    return {
        { .name = "XDG_CACHE_HOME first, where a user set it",
          .environment = { { "XDG_CACHE_HOME", "C:/x/cache" },
                           { "LOCALAPPDATA", "C:/Users/u/AppData/Local" },
                           { "USERPROFILE", "C:/Users/u" } },
          .expected = "C:/x/cache" },
        { .name = "LOCALAPPDATA",
          .environment = { { "LOCALAPPDATA", "C:/Users/u/AppData/Local" }, { "USERPROFILE", "C:/Users/u" } },
          .expected = "C:/Users/u/AppData/Local" },
        { .name = "a drive-relative LOCALAPPDATA is ignored",
          .environment = { { "LOCALAPPDATA", "/Users/u/AppData/Local" }, { "USERPROFILE", "C:/Users/u" } },
          .expected = "C:/Users/u/AppData/Local" },
        { .name = "the OS row: AppData/Local below the profile",
          .environment = { { "USERPROFILE", "C:/Users/u" } },
          .expected = "C:/Users/u/AppData/Local" },
        { .name = "a relative profile names no cache",
          .environment = { { "USERPROFILE", "Users/u" } },
          .expected = std::nullopt },
        { .name = "nothing set", .environment = {}, .expected = std::nullopt },
    };
}

} // namespace core::platform::test
