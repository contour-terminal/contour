// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
    #include <sys/stat.h>
#endif

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

TEST_CASE("DirectoryHandle: without a path from the descriptor, the canonical path is walked to",
          "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const real = directory.path() / "real";
    std::filesystem::create_directory(real);
    std::filesystem::create_directory_symlink(real, directory.path() / "link");
    auto const opener = vtpty::makeDirectoryOpener(vtpty::DescriptorPaths::Unavailable);

    auto const opened = opener->open((directory.path() / "link").string());
    REQUIRE(opened.has_value());
    auto const canonical = std::filesystem::canonical(real);
    CHECK(opened->resolvedPath() == canonical.string());
    // The descriptor is the directory the path names: the walk opened it, not the name.
    struct stat held {};
    struct stat named {};
    REQUIRE(::fstat(opened->descriptor(), &held) == 0);
    REQUIRE(::stat(canonical.c_str(), &named) == 0);
    CHECK(held.st_ino == named.st_ino);
    CHECK(held.st_dev == named.st_dev);

    auto const created = opener->createBeneath(*opened, "child");
    REQUIRE(created.has_value());
    CHECK(created->resolvedPath() == (canonical / "child").string());
}

TEST_CASE("DirectoryHandle: createBeneath creates one directory and follows no link", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto& opener = vtpty::nativeDirectoryOpener();
    auto const parent = opener.open(directory.path().string());
    REQUIRE(parent.has_value());

    auto const created = opener.createBeneath(*parent, "x");
    REQUIRE(created.has_value());
    CHECK(std::filesystem::is_directory(directory.path() / "x"));
    CHECK(created->resolvedPath() == (std::filesystem::canonical(directory.path()) / "x").string());
    // Again: it exists, and is opened.
    CHECK(opener.createBeneath(*parent, "x").has_value());

    std::ofstream { directory.path() / "file" } << "x";
    CHECK(opener.createBeneath(*parent, "file").error() == vtpty::DirectoryError::NotADirectory);

    std::filesystem::create_directory(directory.path() / "elsewhere");
    std::filesystem::create_directory_symlink(directory.path() / "elsewhere", directory.path() / "link");
    CHECK(opener.createBeneath(*parent, "link").error() == vtpty::DirectoryError::NotADirectory);

    for (auto const* const name: { "", ".", "..", "a/b" })
        CHECK(opener.createBeneath(*parent, name).error() == vtpty::DirectoryError::AccessDenied);
    CHECK_FALSE(std::filesystem::exists(directory.path() / "a"));
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
