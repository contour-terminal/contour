// SPDX-License-Identifier: Apache-2.0
//
// cacheHome's rows on Linux, FreeBSD and every other POSIX system but macOS: ~/.cache below HOME.
#include <core/platform/UserPathsTestSupport.hpp>

#include <vector>

namespace core::platform::test
{

std::vector<CacheHomeCase> cacheHomeCases()
{
    return {
        { .name = "XDG_CACHE_HOME first",
          .environment = { { "XDG_CACHE_HOME", "/x/cache" },
                           { "LOCALAPPDATA", "/x/local" },
                           { "HOME", "/x/home" } },
          .expected = "/x/cache" },
        { .name = "an empty XDG_CACHE_HOME is unset: LOCALAPPDATA",
          .environment = { { "XDG_CACHE_HOME", "" }, { "LOCALAPPDATA", "/x/local" }, { "HOME", "/x/home" } },
          .expected = "/x/local" },
        { .name = "a relative XDG_CACHE_HOME is ignored, as the XDG specification says",
          .environment = { { "XDG_CACHE_HOME", "cache" }, { "HOME", "/x/home" } },
          .expected = "/x/home/.cache" },
        { .name = "a relative LOCALAPPDATA is ignored too",
          .environment = { { "LOCALAPPDATA", "local" }, { "HOME", "/x/home" } },
          .expected = "/x/home/.cache" },
        { .name = "the OS row: ~/.cache",
          .environment = { { "HOME", "/x/home" } },
          .expected = "/x/home/.cache" },
        { .name = "a relative HOME names no cache",
          .environment = { { "HOME", "home" } },
          .expected = std::nullopt },
        { .name = "nothing set", .environment = {}, .expected = std::nullopt },
    };
}

} // namespace core::platform::test
