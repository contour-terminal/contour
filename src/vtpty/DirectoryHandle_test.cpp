// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

using namespace std::chrono_literals;

#ifndef _WIN32

TEST_CASE("DirectoryHandle: a handle keeps its directory when the name is replaced", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const named = directory.path() / "a";
    std::filesystem::create_directory(named);
    auto opened = vtpty::nativeDirectoryOpener().open(named.string());
    REQUIRE(opened.has_value());

    std::filesystem::rename(named, directory.path() / "a.old");
    std::filesystem::create_directory_symlink("/", named);

    auto clock = core::platform::ManualClock {};
    auto created = vtpty::PipedProcessRunner::create(clock, {});
    REQUIRE(created.has_value());
    auto& runner = **created;
    auto printed = std::optional<std::string> {};
    auto const started = runner.start(vtpty::ProcessSpec { .argv = { "sh", "-c", "pwd -P" },
                                                           .workingDirectory = std::move(*opened),
                                                           .environment = { { "PATH", "/usr/bin:/bin" } },
                                                           .timeout = 30s,
                                                           .outputCap = std::size_t { 4096 } },
                                      [&printed](vtpty::ProcessToken, vtpty::ProcessExit exit) {
                                          printed = std::move(exit.standardOutput.bytes);
                                      });
    REQUIRE(started.has_value());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
    {
        if (printed)
            break;
        runner.pump(100ms);
    }
    REQUIRE(printed.has_value());
    CHECK(*printed == std::filesystem::canonical(directory.path() / "a.old").string() + "\n");
}

TEST_CASE("DirectoryHandle: a missing directory", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const opened = vtpty::nativeDirectoryOpener().open((directory.path() / "missing").string());
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == vtpty::DirectoryError::Missing);
}

TEST_CASE("DirectoryHandle: a file", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const file = directory.path() / "file";
    std::ofstream { file } << "x";
    auto const opened = vtpty::nativeDirectoryOpener().open(file.string());
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == vtpty::DirectoryError::NotADirectory);
}

TEST_CASE("DirectoryHandle: resolvedPath follows symlinks", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const real = directory.path() / "real";
    std::filesystem::create_directory(real);
    std::filesystem::create_directory_symlink(real, directory.path() / "link");

    auto const opened = vtpty::nativeDirectoryOpener().open((directory.path() / "link").string());
    REQUIRE(opened.has_value());
    CHECK(opened->descriptor() >= 0);
    CHECK(opened->resolvedPath() == std::filesystem::canonical(real).string());

    auto const canonical = vtpty::nativeDirectoryOpener().canonical((directory.path() / "link").string());
    REQUIRE(canonical.has_value());
    CHECK(*canonical == std::filesystem::canonical(real).string());
}

TEST_CASE("DirectoryHandle: ensureExists creates every missing parent", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const deep = directory.path() / "x" / "y" / "z";
    REQUIRE(vtpty::nativeDirectoryOpener().ensureExists(deep.string()).has_value());
    CHECK(std::filesystem::is_directory(deep));
    // Again: it exists, which is all that is asked.
    CHECK(vtpty::nativeDirectoryOpener().ensureExists(deep.string()).has_value());

    auto const file = directory.path() / "file";
    std::ofstream { file } << "x";
    auto const blocked = vtpty::nativeDirectoryOpener().ensureExists(file.string());
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error() == vtpty::DirectoryError::NotADirectory);
}

TEST_CASE("DirectoryHandle: an unbacked handle holds no descriptor", "[processrunner]")
{
    auto const handle = vtpty::DirectoryHandle::unbacked("/somewhere");
    CHECK(handle.descriptor() == -1);
    CHECK(handle.resolvedPath() == "/somewhere");
}

#else

TEST_CASE("DirectoryHandle: opening is unsupported", "[processrunner]")
{
    auto const opened = vtpty::nativeDirectoryOpener().open("C:\\");
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == vtpty::DirectoryError::Unsupported);
}

#endif
