// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessEnvironment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace
{

/// @return Whether @p entries holds a variable named @p name, the name compared as the platform compares it.
[[nodiscard]] bool hasVariable(std::vector<std::string> const& entries, std::string_view name)
{
    return std::ranges::any_of(entries, [name](std::string const& entry) {
        auto const equals = entry.find('=');
        if (equals != name.size())
            return false;
#ifdef _WIN32
        return std::ranges::equal(std::string_view { entry }.substr(0, equals), name, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
#else
        return std::string_view { entry }.substr(0, equals) == name;
#endif
    });
}

} // namespace

TEST_CASE("processEnvironmentEntries holds this process's PATH, as NAME=value", "[vtpty][environment]")
{
    auto const entries = vtpty::processEnvironmentEntries();
    REQUIRE_FALSE(entries.empty());
    CHECK(hasVariable(entries, "PATH"));
    CHECK(std::ranges::all_of(entries, [](std::string const& entry) { return entry.find('=') != 0; }));
#ifdef _WIN32
    // A program such as Claude Code (Bun) refuses network requests without it; git needs it too.
    CHECK(hasVariable(entries, "SystemRoot"));
#endif
}
