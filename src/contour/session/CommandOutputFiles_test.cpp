// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandOutputFiles.hpp>

#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <utility>

using contour::session::CommandOutputFiles;
using contour::session::OutputFileError;
using contour::session::ProcessLiveness;
using contour::session::ProcessState;
using core::platform::testing::FileEntry;
using core::platform::testing::InMemoryFileSystem;
namespace fs = std::filesystem;

namespace
{
/// The private directory the tests hand to CommandOutputFiles.
[[nodiscard]] fs::path privateDirectory()
{
    return { "/run/contour-output-42" };
}

/// What the tests tell CommandOutputFiles about other processes: what they were given, else nothing.
class KnownProcesses final: public ProcessLiveness
{
  public:
    explicit KnownProcesses(std::map<int64_t, ProcessState> states): _states { std::move(states) } {}

    [[nodiscard]] ProcessState stateOf(int64_t processId) const override
    {
        auto const known = _states.find(processId);
        return known != _states.end() ? known->second : ProcessState::Unknown;
    }

  private:
    std::map<int64_t, ProcessState> _states;
};

/// Knows of no process at all.
class UnknownProcesses final: public ProcessLiveness
{
  public:
    [[nodiscard]] ProcessState stateOf(int64_t /*processId*/) const override { return ProcessState::Unknown; }
};

UnknownProcesses const noneKnown {};
} // namespace

TEST_CASE("CommandOutputFiles: files live in an owner-only directory made on first use",
          "[contour][session][semanticblocks]")
{
    auto const fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
    {
        auto files = CommandOutputFiles { fileSystem, noneKnown, privateDirectory() };
        CHECK_FALSE(fileSystem.exists(privateDirectory())); // nothing until a file is needed

        auto const first = files.create("hello");
        REQUIRE(first.has_value());
        CHECK(first->parent_path() == privateDirectory());
        CHECK(fileSystem.readFile(*first).value_or("") == "hello");
        CHECK(fileSystem.permissions(privateDirectory()).value_or(fs::perms::none) == fs::perms::owner_all);
        CHECK(fileSystem.permissions(*first).value_or(fs::perms::none)
              == (fs::perms::owner_read | fs::perms::owner_write));

        auto const second = files.create("world");
        REQUIRE(second.has_value());
        CHECK(*second != *first);
    }
    CHECK_FALSE(fileSystem.exists(privateDirectory())); // gone with the object, files and all
}

TEST_CASE("CommandOutputFiles: release deletes only the files it handed out",
          "[contour][session][semanticblocks]")
{
    auto const fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
    auto files = CommandOutputFiles { fileSystem, noneKnown, privateDirectory() };
    auto const file = files.create("x");
    REQUIRE(file.has_value());

    files.release(*file);
    CHECK_FALSE(fileSystem.exists(*file));

    REQUIRE(fileSystem.writeFile("/run/other.txt", "keep").has_value());
    files.release("/run/other.txt");
    CHECK(fileSystem.exists("/run/other.txt"));
}

TEST_CASE("CommandOutputFiles: a directory removed since its first use is made again",
          "[contour][session][semanticblocks]")
{
    // A temporary-file cleaner, tmpfiles ageing or a user removing it: until Contour restarted, every
    // later file failed to be written. The real filesystem, because the in-memory one makes a missing
    // parent directory for any file written into it.
    auto const scratch = core::testing::ScopedTempDir { "contour-output-files" };
    auto const directory = scratch.path() / "contour-output-test";
    auto files = CommandOutputFiles { core::platform::NativeFileSystem::instance(), noneKnown, directory };
    REQUIRE(files.create("first").has_value());
    fs::remove_all(directory);
    REQUIRE_FALSE(fs::exists(directory));

    auto const second = files.create("second");
    REQUIRE(second.has_value());
    CHECK(core::platform::NativeFileSystem::instance().readFile(*second).value_or("") == "second");
}

TEST_CASE("CommandOutputFiles: what a process that is gone left behind is removed on first use",
          "[contour][session][semanticblocks]")
{
    // A crash skips the removal at exit, and Windows never empties %TEMP%: the output, secrets and all,
    // stayed on disk for good.
    auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
    for (auto const* name: { "contour-output-7", "contour-output-8", "contour-output-9", "contour-output-x" })
    {
        fileSystem.addDirectory(fs::path("/run") / name);
        REQUIRE(fileSystem.writeFile(fs::path("/run") / name / "output-1.txt", "secret").has_value());
    }
    fileSystem.addDirectory("/run/unrelated-7");
    fileSystem.addSymlink("/run/contour-output-6", "/run/unrelated-7");
    auto const processes = KnownProcesses { { { 6, ProcessState::Gone },
                                              { 7, ProcessState::Gone },
                                              { 8, ProcessState::Running },
                                              { 9, ProcessState::Unknown } } };

    auto files = CommandOutputFiles { fileSystem, processes, privateDirectory() };
    CHECK(fileSystem.exists("/run/contour-output-7")); // nothing is looked at before a file is needed
    REQUIRE(files.create("x").has_value());

    CHECK_FALSE(fileSystem.exists("/run/contour-output-7")); // its process is gone
    CHECK(fileSystem.exists("/run/contour-output-8"));       // its process runs
    CHECK(fileSystem.exists("/run/contour-output-9"));       // in doubt, kept
    CHECK(fileSystem.exists("/run/contour-output-x"));       // no process's
    CHECK(fileSystem.isSymlink("/run/contour-output-6"));    // a link is never followed or removed
    CHECK(fileSystem.exists("/run/unrelated-7"));
}

TEST_CASE("CommandOutputFiles: a stale directory is replaced, a link is never used",
          "[contour][session][semanticblocks]")
{
    SECTION("a directory an earlier process with this pid left behind")
    {
        auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
        fileSystem.addDirectory(privateDirectory());
        REQUIRE(fileSystem.writeFile(privateDirectory() / "old.txt", "stale").has_value());

        auto files = CommandOutputFiles { fileSystem, noneKnown, privateDirectory() };
        REQUIRE(files.create("new").has_value());
        CHECK_FALSE(fileSystem.exists(privateDirectory() / "old.txt"));
    }

    SECTION("a symlink in its place")
    {
        auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
        fileSystem.addSymlink(privateDirectory(), "/run/somewhere-else");

        auto files = CommandOutputFiles { fileSystem, noneKnown, privateDirectory() };
        CHECK(files.create("secret") == std::unexpected(OutputFileError::DirectoryUnavailable));
    }

    SECTION("no parent to create it in, or no place at all")
    {
        auto const fileSystem = InMemoryFileSystem {};
        CHECK(CommandOutputFiles { fileSystem, noneKnown, privateDirectory() }.create("x")
              == std::unexpected(OutputFileError::DirectoryUnavailable));
        CHECK(CommandOutputFiles { fileSystem, noneKnown, fs::path {} }.create("x")
              == std::unexpected(OutputFileError::DirectoryUnavailable));
    }
}

#ifndef _WIN32
TEST_CASE("CommandOutputFiles: owner-only on the real filesystem", "[contour][session][semanticblocks]")
{
    // The in-memory filesystem answers owner read/write for any file it was not told otherwise about,
    // so only the real one shows that the file's own permissions are narrowed, not just the directory's.
    auto const scratch = core::testing::ScopedTempDir { "contour-output-files" };
    auto const directory = scratch.path() / "contour-output-test";
    {
        auto files =
            CommandOutputFiles { core::platform::NativeFileSystem::instance(), noneKnown, directory };
        auto const file = files.create("secret");
        REQUIRE(file.has_value());
        CHECK((fs::status(directory).permissions() & fs::perms::all) == fs::perms::owner_all);
        CHECK((fs::status(*file).permissions() & fs::perms::all)
              == (fs::perms::owner_read | fs::perms::owner_write));
    }
    CHECK_FALSE(fs::exists(directory));
}
#endif

TEST_CASE("commandOutputDirectory: the user's runtime directory first, else the temporary one",
          "[contour][session][semanticblocks]")
{
    using contour::session::commandOutputDirectory;

    auto const host = vtpty::SandboxInfo {};
    auto const runtime = core::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "/run/user/1000" } } };
    CHECK(commandOutputDirectory(runtime, host, "/tmp", 42)
          == fs::path("/run/user/1000") / "contour-output-42");

    // The XDG base directory specification: a relative path in the variable is invalid, and ignored.
    auto const relative = core::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "run/user/1000" } } };
    CHECK(commandOutputDirectory(relative, host, "/tmp", 42) == fs::path("/tmp") / "contour-output-42");

    auto const none = core::testing::FakeEnvironment {};
    CHECK(commandOutputDirectory(none, host, "/tmp", 42) == fs::path("/tmp") / "contour-output-42");
    CHECK(commandOutputDirectory(none, host, fs::path {}, 42).empty());

    // A Flatpak's pager pane runs on the host, which cannot see the sandbox's own runtime directory:
    // the files go where flatpak shares one with the host, at the same path on both sides.
    auto const flatpak = vtpty::SandboxInfo { .state = vtpty::SandboxState::Flatpak,
                                              .network = vtpty::NetworkAccess::Permitted,
                                              .applicationId = "org.contourterminal.Contour" };
    CHECK(commandOutputDirectory(runtime, flatpak, "/tmp", 42)
          == fs::path("/run/user/1000") / "app" / "org.contourterminal.Contour" / "contour-output-42");
}

TEST_CASE("describe(OutputFileError) names every cause", "[contour][session][semanticblocks]")
{
    using contour::session::describe;
    STATIC_CHECK(!describe(OutputFileError::DirectoryUnavailable).empty());
    STATIC_CHECK(!describe(OutputFileError::WriteFailed).empty());
    STATIC_CHECK(!describe(OutputFileError::PermissionsFailed).empty());
}
