// SPDX-License-Identifier: Apache-2.0
#include <core/platform/GlobMatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using core::platform::containsGlobChars;
using core::platform::globMatchFilename;
using namespace std::string_view_literals;

namespace
{
struct Row
{
    std::string_view filename;
    std::string_view pattern;
    bool matches;
};

constexpr auto Rows = std::array {
    // Literal text matches only itself.
    Row { .filename = "readme.md"sv, .pattern = "readme.md"sv, .matches = true },
    Row { .filename = "readme.md"sv, .pattern = "readme.txt"sv, .matches = false },
    Row { .filename = ""sv, .pattern = ""sv, .matches = true },
    // `*` matches any run, the empty one included, and backtracks.
    Row { .filename = "a.txt"sv, .pattern = "*.txt"sv, .matches = true },
    Row { .filename = ".txt"sv, .pattern = "*.txt"sv, .matches = true },
    Row { .filename = "a.txt.bak"sv, .pattern = "*.txt"sv, .matches = false },
    Row { .filename = "a.b.c"sv, .pattern = "*.c"sv, .matches = true },
    Row { .filename = "abcabd"sv, .pattern = "*abd"sv, .matches = true },
    Row { .filename = "anything"sv, .pattern = "*"sv, .matches = true },
    Row { .filename = ""sv, .pattern = "*"sv, .matches = true },
    Row { .filename = "abc"sv, .pattern = "a**c"sv, .matches = true },
    // `?` matches exactly one character.
    Row { .filename = "ab.txt"sv, .pattern = "??.txt"sv, .matches = true },
    Row { .filename = "abc.txt"sv, .pattern = "??.txt"sv, .matches = false },
    Row { .filename = "a.txt"sv, .pattern = "??.txt"sv, .matches = false },
    // A bracket expression matches one character from a set or a range.
    Row { .filename = "b.txt"sv, .pattern = "[abc].txt"sv, .matches = true },
    Row { .filename = "d.txt"sv, .pattern = "[abc].txt"sv, .matches = false },
    Row { .filename = "file5"sv, .pattern = "file[0-9]"sv, .matches = true },
    Row { .filename = "filex"sv, .pattern = "file[0-9]"sv, .matches = false },
    // `!` or `^` negates it.
    Row { .filename = "d.txt"sv, .pattern = "[!abc].txt"sv, .matches = true },
    Row { .filename = "a.txt"sv, .pattern = "[^abc].txt"sv, .matches = false },
    // A bracket that fails under a `*` lets the star take one more character.
    Row { .filename = "x1y2"sv, .pattern = "*[0-9]"sv, .matches = true },
    // A bracket expression holding a `[` is POSIX's way to match a literal bracket, so the
    // bracket arm has to be reached before the literal one -- which it was not.
    Row { .filename = "["sv, .pattern = "[[]"sv, .matches = true },
    Row { .filename = "a"sv, .pattern = "[[]"sv, .matches = false },
    Row { .filename = "[x"sv, .pattern = "[[]x"sv, .matches = true },
    // A `[` that no `]` closes is a literal `[`, as fnmatch(3) reads it.
    Row { .filename = "["sv, .pattern = "["sv, .matches = true },
    Row { .filename = "[abc"sv, .pattern = "[abc"sv, .matches = true },
    Row { .filename = "a[b"sv, .pattern = "a[b"sv, .matches = true },
};
} // namespace

TEST_CASE("globMatchFilename matches a filename against a shell pattern", "[platform][glob]")
{
    for (auto const& row: Rows)
    {
        INFO(row.filename << " against " << row.pattern);
        CHECK(globMatchFilename(row.filename, row.pattern) == row.matches);
    }
}

TEST_CASE("containsGlobChars finds the pattern metacharacters", "[platform][glob]")
{
    STATIC_CHECK(containsGlobChars("*.md"));
    STATIC_CHECK(containsGlobChars("file?.txt"));
    STATIC_CHECK(containsGlobChars("[ab].txt"));
    STATIC_CHECK_FALSE(containsGlobChars("plain/path.txt"));
    STATIC_CHECK_FALSE(containsGlobChars(""));
}
