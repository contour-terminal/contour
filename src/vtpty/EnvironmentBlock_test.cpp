// SPDX-License-Identifier: Apache-2.0
#include <vtpty/EnvironmentBlock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("buildEnvironmentBlock: removes, overrides, keeps the rest", "[process]")
{
    auto const inherited =
        std::vector<std::string_view> { "HOME=/home/u", "DRI_PRIME=8086:a788", "PATH=/bin", "NOEQUALS" };
    auto const removed = std::vector<std::string> { "DRI_PRIME" };
    auto block =
        vtpty::buildEnvironmentBlock(inherited, { { "PATH", "/usr/bin" }, { "TERM", "contour" } }, removed);
    std::ranges::sort(block);
    CHECK(block == std::vector<std::string> { "HOME=/home/u", "NOEQUALS", "PATH=/usr/bin", "TERM=contour" });
}

TEST_CASE("buildEnvironmentBlock: an override wins over a removal of the same name", "[process]")
{
    auto const inherited = std::vector<std::string_view> { "X=1" };
    auto const removed = std::vector<std::string> { "X" };
    CHECK(vtpty::buildEnvironmentBlock(inherited, { { "X", "2" } }, removed)
          == std::vector<std::string> { "X=2" });
}
