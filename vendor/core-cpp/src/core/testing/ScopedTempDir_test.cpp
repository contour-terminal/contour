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
