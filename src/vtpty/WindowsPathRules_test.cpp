// SPDX-License-Identifier: Apache-2.0
//
// The Win32 path rules a directory handle obeys: which component names are addressed by their own
// spelling, and the verbatim prefix that lifts MAX_PATH. Pure, so tested on every platform.

#include <vtpty/WindowsPathRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("isPlainWindowsName accepts ordinary names", "[windowspathrules]")
{
    for (auto const name: { "x",
                            "repo-1",
                            "a.b",
                            ".git",
                            "CONX",
                            "con-d",
                            "COM10",
                            "LPT",
                            "\xc3\x9c"
                            "ber",
                            " lead",
                            "a b" })
        CHECK(vtpty::isPlainWindowsName(name));
}

TEST_CASE("isPlainWindowsName refuses names Win32 spells differently", "[windowspathrules]")
{
    using namespace std::string_view_literals;
    for (auto const name: { ""sv,
                            "."sv,
                            ".."sv,
                            "a/b"sv,
                            "a\\b"sv,
                            "x:stream"sv,
                            "x::$INDEX_ALLOCATION"sv,
                            "C:"sv,
                            "trail."sv,
                            "trail "sv,
                            "a<b"sv,
                            "a>b"sv,
                            "a\"b"sv,
                            "a|b"sv,
                            "a?b"sv,
                            "a*b"sv,
                            "a\0b"sv,
                            "\x01"sv,
                            "a\x1f"sv,
                            "CON"sv,
                            "con"sv,
                            "Nul.txt"sv,
                            "nul .txt"sv,
                            "AUX"sv,
                            "PRN.x.y"sv,
                            "COM1"sv,
                            "com9"sv,
                            "LPT0"sv,
                            "COM\xc2\xb9"sv,
                            "CONIN$"sv,
                            "conout$"sv })
    {
        CAPTURE(name);
        CHECK_FALSE(vtpty::isPlainWindowsName(name));
    }
}

TEST_CASE("dosPathOf drops the verbatim prefix", "[windowspathrules]")
{
    CHECK(vtpty::dosPathOf(R"(\\?\C:\a\b)") == R"(C:\a\b)");
    CHECK(vtpty::dosPathOf(R"(\\?\UNC\srv\share\a)") == R"(\\srv\share\a)");
    CHECK(vtpty::dosPathOf(R"(C:\a)") == R"(C:\a)");
    CHECK(vtpty::dosPathOf(R"(\\srv\share)") == R"(\\srv\share)");
}

TEST_CASE("verbatimPathOf adds the verbatim prefix", "[windowspathrules]")
{
    CHECK(vtpty::verbatimPathOf(R"(C:\a)") == R"(\\?\C:\a)");
    CHECK(vtpty::verbatimPathOf(R"(\\srv\share\a)") == R"(\\?\UNC\srv\share\a)");
    CHECK(vtpty::verbatimPathOf(R"(\\?\C:\a)") == R"(\\?\C:\a)");
    CHECK(vtpty::verbatimPathOf(R"(\\.\pipe\x)") == R"(\\.\pipe\x)");
}
