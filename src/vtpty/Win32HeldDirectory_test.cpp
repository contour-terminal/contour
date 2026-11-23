// SPDX-License-Identifier: Apache-2.0
//
// The check that a held directory may be a Windows child's current directory: that its resolved path names
// it, before and after the child is created, and is short enough to be a current directory at all.
#ifdef _WIN32

    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/UniqueHandle.hpp>
    #include <vtpty/Win32HeldDirectory.hpp>
    #include <vtpty/WindowsProcessRules.hpp>
    #include <vtpty/testing/Junction.hpp>
    #include <vtpty/testing/LongDirectory.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/WideString.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <filesystem>
    #include <string>
    #include <utility>

    #include <Windows.h>

using vtpty::confirmHeldDirectoryEntry;
using vtpty::HeldDirectoryEntryError;
using vtpty::prepareHeldDirectoryEntry;
using vtpty::testing::createJunction;
using vtpty::testing::LongDirectory;

namespace
{

/// @return The directory at @p path, held open by the native opener.
[[nodiscard]] vtpty::DirectoryHandle hold(std::filesystem::path const& path)
{
    auto opened = vtpty::nativeDirectoryOpener().open(core::pathToUtf8(path));
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// A temporary directory holding `work`, the directory each case holds.
struct HeldFixture
{
    HeldFixture() { std::filesystem::create_directories(work); }

    core::testing::ScopedTempDir directory { "contour-held" };
    std::filesystem::path work = directory.path() / "work";
};

} // namespace

TEST_CASE("HeldDirectoryEntry: a held directory is its own entry", "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const held = hold(fixture.work);
    auto const entry = prepareHeldDirectoryEntry(held);
    REQUIRE(entry.has_value());
    CHECK(entry->path == core::toUtf16(held.resolvedPath()).value_or(L""));
    CHECK(confirmHeldDirectoryEntry(*entry).has_value());
}

TEST_CASE("HeldDirectoryEntry: a directory replaced at its name is refused", "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const held = hold(fixture.work);
    auto const old = fixture.directory.path() / "work.old";
    std::filesystem::rename(fixture.work, old);

    SECTION("a new directory at its name")
    {
        std::filesystem::create_directories(fixture.work);
        CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::Replaced);
    }
    SECTION("a junction at its name, to another directory")
    {
        auto const elsewhere = fixture.directory.path() / "elsewhere";
        std::filesystem::create_directories(elsewhere);
        REQUIRE(createJunction(fixture.work, elsewhere).has_value());
        CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::Replaced);
    }
    SECTION("a junction at its name, back to the held directory")
    {
        // Leads to the held directory now, but a junction can be retargeted after the check: only the
        // directory itself at its name is guarded.
        REQUIRE(createJunction(fixture.work, old).has_value());
        CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::Replaced);
    }
}

TEST_CASE("HeldDirectoryEntry: an entry keeps its directory from being renamed or removed until released",
          "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const old = fixture.directory.path() / "work.old";
    {
        auto const held = hold(fixture.work);
        auto const entry = prepareHeldDirectoryEntry(held);
        REQUIRE(entry.has_value());
        CHECK(MoveFileExW(fixture.work.c_str(), old.c_str(), 0) == FALSE);
        CHECK(RemoveDirectoryW(fixture.work.c_str()) == FALSE);
        CHECK(std::filesystem::is_directory(fixture.work));
    }
    CHECK(MoveFileExW(fixture.work.c_str(), old.c_str(), 0) != FALSE);
}

TEST_CASE("HeldDirectoryEntry: the confirmation refuses a path that names another directory",
          "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const other = fixture.directory.path() / "other";
    std::filesystem::create_directories(other);
    auto const held = hold(fixture.work);
    auto const entry = prepareHeldDirectoryEntry(held);
    REQUIRE(entry.has_value());

    auto const elsewhere =
        vtpty::HeldDirectoryEntry { .path = core::toUtf16(hold(other).resolvedPath()).value_or(L""),
                                    .identity = entry->identity,
                                    .guard = {} };
    CHECK(confirmHeldDirectoryEntry(elsewhere).error() == HeldDirectoryEntryError::Replaced);
}

TEST_CASE(
    "HeldDirectoryEntry: a child created suspended runs in the held directory, its name attacked meanwhile",
    "[processrunner][held]")
{
    // CreateProcessW() records the current directory as a path, which the child's loader opens only once it
    // is resumed: the name swapped in between would be entered, were the entry not guarding it.
    auto const fixture = HeldFixture {};
    auto const held = hold(fixture.work);
    auto const entry = prepareHeldDirectoryEntry(held);
    REQUIRE(entry.has_value());

    auto const program = std::filesystem::path { VTPTY_PROCESS_RUNNER_PROBE };
    auto commandLine = std::wstring { L"probe touch marker" };
    auto startup = STARTUPINFOW {};
    startup.cb = sizeof(startup);
    auto info = PROCESS_INFORMATION {};
    REQUIRE(CreateProcessW(program.c_str(),
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           nullptr,
                           entry->path.c_str(),
                           &startup,
                           &info)
            != FALSE);
    auto const process = vtpty::UniqueHandle { info.hProcess };
    auto const thread = vtpty::UniqueHandle { info.hThread };

    auto const old = fixture.directory.path() / "work.old";
    auto const renamed = MoveFileExW(fixture.work.c_str(), old.c_str(), 0) != FALSE;
    if (renamed)
        std::filesystem::create_directories(fixture.work);
    CHECK_FALSE(renamed);

    REQUIRE(ResumeThread(thread.get()) != static_cast<DWORD>(-1));
    REQUIRE(WaitForSingleObject(process.get(), 30000) == WAIT_OBJECT_0);
    auto code = DWORD { 1 };
    REQUIRE(GetExitCodeProcess(process.get(), &code) != FALSE);
    CHECK(code == 0);
    CHECK(std::filesystem::exists((renamed ? old : fixture.work) / "marker"));
}

TEST_CASE("HeldDirectoryEntry: a directory another handle may delete cannot be guarded",
          "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const held = hold(fixture.work);
    auto const deleter =
        vtpty::UniqueHandle { CreateFileW(fixture.work.c_str(),
                                          DELETE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                          nullptr) };
    REQUIRE(deleter.valid());
    CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::Unguardable);
}

TEST_CASE("HeldDirectoryEntry: a removed directory is refused", "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const held = hold(fixture.work);
    std::filesystem::remove(fixture.work);
    CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::Removed);
}

TEST_CASE("HeldDirectoryEntry: an unbacked handle is refused", "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const held = vtpty::DirectoryHandle::unbacked(core::pathToUtf8(fixture.work));
    CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::Unbacked);
}

TEST_CASE("HeldDirectoryEntry: a resolved path that is not UTF-8 is refused", "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto handle = vtpty::UniqueHandle { CreateFileW(fixture.work.c_str(),
                                                    FILE_READ_ATTRIBUTES,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                    nullptr,
                                                    OPEN_EXISTING,
                                                    FILE_FLAG_BACKUP_SEMANTICS,
                                                    nullptr) };
    REQUIRE(handle.valid());
    auto const held = vtpty::DirectoryHandle { std::move(handle), "C:\\\xff" };
    CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::NotUtf8);
}

TEST_CASE("HeldDirectoryEntry: a path too long to be a current directory is refused, not cut",
          "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const base = core::toUtf16(hold(fixture.directory.path()).resolvedPath());
    REQUIRE(base.has_value());

    SECTION("one unit longer than a current directory may be")
    {
        auto const chain = LongDirectory::make(*base, vtpty::MaxWindowsCurrentDirectoryUnits + 1, L'a');
        REQUIRE(chain.has_value());
        auto const held = hold(chain->path());
        REQUIRE(core::toUtf16(held.resolvedPath()).value_or(L"").size()
                == vtpty::MaxWindowsCurrentDirectoryUnits + 1);
        CHECK(prepareHeldDirectoryEntry(held).error() == HeldDirectoryEntryError::TooLong);
    }
    SECTION("exactly as long as a current directory may be")
    {
        auto const chain = LongDirectory::make(*base, vtpty::MaxWindowsCurrentDirectoryUnits, L'b');
        REQUIRE(chain.has_value());
        auto const held = hold(chain->path());
        REQUIRE(core::toUtf16(held.resolvedPath()).value_or(L"").size()
                == vtpty::MaxWindowsCurrentDirectoryUnits);
        auto const entry = prepareHeldDirectoryEntry(held);
        REQUIRE(entry.has_value());
        CHECK(entry->path.size() == vtpty::MaxWindowsCurrentDirectoryUnits);
    }
}

#endif
