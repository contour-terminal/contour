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
        // The same file id: a child entering the path enters the held directory, so it is not refused.
        REQUIRE(createJunction(fixture.work, old).has_value());
        CHECK(prepareHeldDirectoryEntry(held).has_value());
    }
}

TEST_CASE("HeldDirectoryEntry: a directory replaced after it was prepared is refused at the confirmation",
          "[processrunner][held]")
{
    auto const fixture = HeldFixture {};
    auto const held = hold(fixture.work);
    auto const entry = prepareHeldDirectoryEntry(held);
    REQUIRE(entry.has_value());

    std::filesystem::rename(fixture.work, fixture.directory.path() / "work.old");
    std::filesystem::create_directories(fixture.work);
    CHECK(confirmHeldDirectoryEntry(*entry).error() == HeldDirectoryEntryError::Replaced);
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
