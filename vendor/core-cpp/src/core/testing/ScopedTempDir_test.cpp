// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("ScopedTempDir hands out a distinct directory each time", "[FileSystem]")
{
    // The property the helper exists for. Fixed fixture names looked distinctive too --
    // "my_cp_test_recursive" reads unique until a second process picks the same one --
    // so uniqueness has to come from the OS and be asserted, not assumed.
    auto const first = core::testing::ScopedTempDir { "core_scoped_test" };
    auto const second = core::testing::ScopedTempDir { "core_scoped_test" };

    REQUIRE(!first.path().empty());
    REQUIRE(!second.path().empty());
    CHECK(first.path() != second.path());
    CHECK(std::filesystem::is_directory(first.path()));
    CHECK(std::filesystem::is_directory(second.path()));
}

TEST_CASE("ScopedTempDir removes its directory on destruction", "[FileSystem]")
{
    auto captured = std::filesystem::path {};
    {
        auto const dir = core::testing::ScopedTempDir { "core_scoped_test" };
        captured = dir.path();
        REQUIRE(std::filesystem::is_directory(captured));
        std::ofstream { dir / "leftover.txt" } << "content";
    }
    CHECK(!std::filesystem::exists(captured));
}

TEST_CASE("ScopedTempDir reads its prefix and a name as UTF-8, and spells its path in UTF-8", "[FileSystem]")
{
    // core-cpp's strings are UTF-8. On Windows the narrow constructor read the prefix and the name
    // in the ANSI code page, and generic_string() spelled the path back in it, throwing for a name
    // the code page cannot spell.
    auto const dir = core::testing::ScopedTempDir { "core_scoped_\xCE\xA9" };
    CHECK(dir.path().filename().u8string().starts_with(u8"core_scoped_Ω_"));
    CHECK(dir / "J\xC3\xBCrgen" == dir.path() / std::filesystem::path { u8"Jürgen" });
    CHECK(dir.string().contains("/core_scoped_\xCE\xA9_"));
}
