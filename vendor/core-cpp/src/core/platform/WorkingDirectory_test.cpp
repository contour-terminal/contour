// SPDX-License-Identifier: Apache-2.0
#include <core/platform/WorkingDirectory.hpp>
#include <core/platform/testing/TestWorkingDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <system_error>

using core::platform::PlatformError;
using core::platform::testing::TestWorkingDirectory;

TEST_CASE("TestWorkingDirectory.changeDirectory", "[platform]")
{
    TestWorkingDirectory cwd("/home/user");
    CHECK(cwd.currentDirectory().generic_string() == "/home/user");

    REQUIRE(cwd.changeDirectory("/tmp").has_value());
    CHECK(cwd.currentDirectory().generic_string() == "/tmp");

    REQUIRE(cwd.changeDirectory("sub/../other").has_value());
    CHECK(cwd.currentDirectory().generic_string() == "/tmp/other");
}

TEST_CASE("TestWorkingDirectory.changeDirectory_invalid", "[platform]")
{
    TestWorkingDirectory cwd("/home/user");
    cwd.addValidPath("/allowed");

    auto const result = cwd.changeDirectory("/forbidden");
    REQUIRE(!result.has_value());
    CHECK(result.error() == PlatformError::FileNotFound);
    CHECK(cwd.currentDirectory().generic_string() == "/home/user");
}

TEST_CASE("TestWorkingDirectory holds its directories in UTF-8", "[platform]")
{
    // Its constructor and addValidPath() take UTF-8, as every core-cpp string is. On Windows the
    // narrow constructor read them in the ANSI code page, and generic_string() spelled a directory
    // back in it, which throws for a name the code page cannot spell.
    TestWorkingDirectory cwd("/home/j\xC3\xBCrgen");
    CHECK(cwd.currentDirectory() == std::filesystem::path { u8"/home/jürgen" });

    cwd.addValidPath("/home/j\xC3\xBCrgen/\xCE\xA9");
    REQUIRE(cwd.changeDirectory(std::filesystem::path { u8"Ω" }).has_value());
    CHECK(cwd.currentDirectory() == std::filesystem::path { u8"/home/jürgen/Ω" });
}

TEST_CASE("the native WorkingDirectory is the process's own", "[platform]")
{
    // A working directory is not an environment variable, which is why it has a seam of its own
    // (core-cpp#7): it was two members of the environment interface.
    auto const cwd = core::platform::nativeWorkingDirectory();
    REQUIRE(cwd != nullptr);
    auto const original = std::filesystem::current_path();
    CHECK(std::filesystem::equivalent(cwd->currentDirectory(), original));

    auto const target = std::filesystem::temp_directory_path();
    REQUIRE(cwd->changeDirectory(target).has_value());
    auto ec = std::error_code {};
    CHECK(std::filesystem::equivalent(std::filesystem::current_path(), target, ec));
    CHECK(std::filesystem::equivalent(cwd->currentDirectory(), target, ec));

    REQUIRE(cwd->changeDirectory(original).has_value());
    CHECK(!cwd->changeDirectory(original / "core-cpp-no-such-directory").has_value());
    CHECK(std::filesystem::current_path() == original);
}
