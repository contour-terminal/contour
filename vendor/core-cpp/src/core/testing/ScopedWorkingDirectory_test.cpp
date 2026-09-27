// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ScopedTempDir.hpp>
#include <core/testing/ScopedWorkingDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("ScopedWorkingDirectory moves into a directory and back out of it", "[testing][cwd]")
{
    auto const before = std::filesystem::current_path();
    auto const dir = core::testing::ScopedTempDir { "core_cwd_test" };
    {
        auto const moved = core::testing::ScopedWorkingDirectory { dir.path() };
        CHECK(std::filesystem::equivalent(std::filesystem::current_path(), dir.path()));
    }
    CHECK(std::filesystem::current_path() == before);
}
