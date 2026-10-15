// SPDX-License-Identifier: Apache-2.0
#include <vtpty/EnvironmentBlock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("buildEnvironmentBlock: removes, overrides, keeps the rest", "[process]")
{
    auto const inherited =
        std::vector<std::string_view> { "HOME=/home/u", "DRI_PRIME=8086:a788", "PATH=/bin", "NOEQUALS" };
    auto const removed = std::vector<std::string> { "DRI_PRIME" };
    auto block = vtpty::buildEnvironmentBlock(inherited,
                                              { { "PATH", "/usr/bin" }, { "TERM", "contour" } },
                                              removed,
                                              vtpty::EnvironmentNames::CaseSensitive);
    std::ranges::sort(block);
    CHECK(block == std::vector<std::string> { "HOME=/home/u", "NOEQUALS", "PATH=/usr/bin", "TERM=contour" });
}

TEST_CASE("buildEnvironmentBlock: an override wins over a removal of the same name", "[process]")
{
    auto const inherited = std::vector<std::string_view> { "X=1" };
    auto const removed = std::vector<std::string> { "X" };
    CHECK(vtpty::buildEnvironmentBlock(
              inherited, { { "X", "2" } }, removed, vtpty::EnvironmentNames::CaseSensitive)
          == std::vector<std::string> { "X=2" });
}

TEST_CASE("buildEnvironmentBlock: names compare as the platform compares them", "[process]")
{
    // Windows reads "Path" and "PATH" as one variable, POSIX as two.
    auto const inherited =
        std::vector<std::string_view> { "Path=C:\\Windows", "Temp=C:\\Temp", "=C:=C:\\dir" };
    auto const removed = std::vector<std::string> { "TEMP" };
    auto const overrides = std::map<std::string, std::string> { { "PATH", "C:\\bin" } };

    auto insensitive =
        vtpty::buildEnvironmentBlock(inherited, overrides, removed, vtpty::EnvironmentNames::CaseInsensitive);
    std::ranges::sort(insensitive);
    // The "=C:" entry that says the drive's directory is named "=C:", which nothing here names.
    CHECK(insensitive == std::vector<std::string> { "=C:=C:\\dir", "PATH=C:\\bin" });

    auto sensitive =
        vtpty::buildEnvironmentBlock(inherited, overrides, removed, vtpty::EnvironmentNames::CaseSensitive);
    std::ranges::sort(sensitive);
    CHECK(sensitive
          == std::vector<std::string> { "=C:=C:\\dir", "PATH=C:\\bin", "Path=C:\\Windows", "Temp=C:\\Temp" });
}

TEST_CASE("buildWindowsEnvironment: TERM stays out, PATH goes first, an empty value removes", "[process]")
{
    auto const inherited = std::vector<std::string_view> { "Path=C:\\Windows", "Gone=1", "Kept=1" };
    auto const removed = std::vector<std::string> {};
    auto block = vtpty::buildWindowsEnvironment(
        inherited, { { "PATH", "C:\\bin" }, { "TERM", "contour" }, { "gone", "" }, { "New", "2" } }, removed);
    std::ranges::sort(block);
    CHECK(block == std::vector<std::string> { "Kept=1", "New=2", "PATH=C:\\bin;C:\\Windows" });
}

TEST_CASE("environmentValue: a variable's value, its name compared as asked", "[process]")
{
    auto const entries = std::vector<std::string> { "Path=C:\\Windows", "EMPTY=", "NOEQUALS" };
    using vtpty::EnvironmentNames;
    CHECK(vtpty::environmentValue(entries, "PATH", EnvironmentNames::CaseInsensitive) == "C:\\Windows");
    CHECK_FALSE(vtpty::environmentValue(entries, "PATH", EnvironmentNames::CaseSensitive).has_value());
    CHECK(vtpty::environmentValue(entries, "EMPTY", EnvironmentNames::CaseSensitive) == "");
    CHECK_FALSE(vtpty::environmentValue(entries, "NOEQUALS", EnvironmentNames::CaseSensitive).has_value());
    CHECK_FALSE(vtpty::environmentValue(entries, "MISSING", EnvironmentNames::CaseSensitive).has_value());
}

#ifdef _WIN32
TEST_CASE("windowsEnvironmentBlock: sorted by name, case ignored, each entry and the block ended by NUL",
          "[process]")
{
    // "All strings in the environment block must be sorted alphabetically by name. The sort is
    // case-insensitive, Unicode order, without regard to locale." (Microsoft Learn, "Changing Environment
    // Variables")
    auto const entries = std::vector<std::string> { "b=2", "A=1", "=C:=C:\\", "\xc3\xa4=3" };
    auto const block = vtpty::windowsEnvironmentBlock(entries);
    REQUIRE(block.has_value());
    CHECK(*block == std::wstring { L"=C:=C:\\\0A=1\0b=2\0\u00e4=3\0\0", 21 });

    // Case ignored, "a" comes before "B", where comparing the code units would put "B" first.
    CHECK(vtpty::windowsEnvironmentBlock(std::vector<std::string> { "B=2", "a=1" })
          == std::wstring { L"a=1\0B=2\0\0", 9 });

    CHECK_FALSE(vtpty::windowsEnvironmentBlock(std::vector<std::string> { "BAD=\xff" }).has_value());
}
#endif
