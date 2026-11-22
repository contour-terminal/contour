// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PathContainment.hpp>

#include <core/Utf8Path.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using namespace std::string_view_literals;
using vtpty::isAtOrBeneath;
using vtpty::PathCase;

namespace
{

/// @return @p posix, a path these tests spell the POSIX way, as this platform spells it: itself on POSIX, on
///         drive C: on Windows (a relative path stays relative).
[[nodiscard]] std::filesystem::path nativePath(std::string_view posix)
{
#ifdef _WIN32
    if (posix.starts_with('/'))
        return core::pathFromTrustedUtf8(std::string { "C:" } + std::string { posix });
#endif
    return core::pathFromTrustedUtf8(posix);
}

/// @return isAtOrBeneath() of @p path and @p directory, both spelled the POSIX way, under @p rule.
[[nodiscard]] bool beneath(std::string_view path, std::string_view directory, PathCase rule)
{
    return isAtOrBeneath(nativePath(path), nativePath(directory), rule);
}

} // namespace

TEST_CASE("PathContainment: a path is at or beneath a directory by whole component", "[process]")
{
    auto const [path, directory, rule, expected] =
        GENERATE(table<std::string_view, std::string_view, PathCase, bool>({
            // Itself, beneath, and the same after lexical normalisation, either rule.
            { "/wt/a", "/wt/a", PathCase::Sensitive, true },
            { "/wt/a/", "/wt/a", PathCase::Sensitive, true },
            { "/wt/a", "/wt/a/", PathCase::Sensitive, true },
            { "/wt/a/./b", "/wt/a", PathCase::Sensitive, true },
            { "/wt/a/b/../c", "/wt/a", PathCase::Sensitive, true },
            { "/wt/a/b", "/", PathCase::Sensitive, true },
            { "/", "/", PathCase::Sensitive, true },
            // A prefix is no component; a parent, a sibling or an escape through `..` is not beneath.
            { "/wt/ab", "/wt/a", PathCase::Sensitive, false },
            { "/wt/ab", "/wt/a", PathCase::Insensitive, false },
            { "/wt", "/wt/a", PathCase::Sensitive, false },
            { "/", "/wt/a", PathCase::Sensitive, false },
            { "/wt/a/../ab", "/wt/a", PathCase::Sensitive, false },
            { "/wt/a/../AB", "/wt/a", PathCase::Insensitive, false },
            // Neither may be relative.
            { "wt/a", "/wt/a", PathCase::Sensitive, false },
            { "/wt/a", "wt/a", PathCase::Sensitive, false },
            { "wt/a", "wt/a", PathCase::Sensitive, false },
            // The sensitive rule compares bytes; the insensitive one folds ASCII letters only.
            { "/WT/A", "/wt/a", PathCase::Sensitive, false },
            { "/wt/A/b", "/wt/a", PathCase::Sensitive, false },
            { "/WT/A", "/wt/a", PathCase::Insensitive, true },
            { "/Wt/a/B", "/WT/a/", PathCase::Insensitive, true },
            { "/wt/\xC3\xA4", "/wt/\xC3\x84", PathCase::Insensitive, false }, // ä is not Ä
            { "/wt/\xC3\xA4/b", "/wt/\xC3\xA4", PathCase::Insensitive, true },
        }));
    INFO(path << " in " << directory);
    CHECK(beneath(path, directory, rule) == expected);
}

#ifdef _WIN32
TEST_CASE("PathContainment: a Windows path is beneath by drive, share and whole component", "[process]")
{
    auto const [path, directory, expected] = GENERATE(table<std::string_view, std::string_view, bool>({
        { R"(C:\wt\a)", R"(C:\wt\a)", true },
        { R"(c:\WT\A\b)", R"(C:\wt\a)", true },
        { "C:/wt/a/b", R"(C:\wt\a)", true },
        { R"(C:\wt\a\)", "c:/WT/a/", true },
        { R"(C:\wt\a\.\b)", R"(C:\wt\a)", true },
        { R"(C:\wt\ab)", R"(C:\wt\a)", false },
        { R"(D:\wt\a)", R"(C:\wt\a)", false },
        { R"(\wt\a)", R"(C:\wt\a)", false },  // drive-relative: not absolute
        { R"(C:wt\a)", R"(C:\wt\a)", false }, // relative to the drive's current directory
        { R"(\\server\share\wt\a)", R"(C:\wt\a)", false },
        { R"(C:\)", R"(C:\wt\a)", false },
        { R"(C:\wt\a\..\ab)", R"(C:\wt\a)", false },
        // A share is a root of its own.
        { R"(\\server\share\wt\a\b)", R"(\\SERVER\share\wt\a)", true },
        { R"(\\other\share\wt\a)", R"(\\server\share\wt\a)", false },
    }));
    INFO(path << " in " << directory);
    CHECK(isAtOrBeneath(
              core::pathFromTrustedUtf8(path), core::pathFromTrustedUtf8(directory), vtpty::NativePathCase)
          == expected);
}
#endif
