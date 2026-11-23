// SPDX-License-Identifier: Apache-2.0
//
// The Win32 calls a read beneath a held directory makes, against this machine's filesystem: entries opened
// beneath a handle as themselves, a file re-opened for reading as the same object, and each handle described
// as the read needs it.

#ifdef _WIN32

    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/Win32BeneathRead.hpp>
    #include <vtpty/testing/Junction.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <chrono>
    #include <cstdint>
    #include <expected>
    #include <filesystem>
    #include <fstream>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>

    #include <Windows.h>

namespace
{

using core::pathToUtf8;
using vtpty::BeneathEntry;

/// Writes @p text to @p path, replacing what was there.
void writeFile(std::filesystem::path const& path, std::string_view text)
{
    auto out = std::ofstream { path, std::ios::binary };
    out << text;
}

/// A temporary directory holding `root` (held open, as a read's root is) and `outside` (beside it).
struct Fixture
{
    core::testing::ScopedTempDir directory { "contour-win32beneath" };
    std::filesystem::path rootPath = directory.path() / "root";
    std::filesystem::path outsidePath = directory.path() / "outside";
    vtpty::DirectoryHandle root = openRoot();

    [[nodiscard]] vtpty::DirectoryHandle openRoot() const
    {
        std::filesystem::create_directory(rootPath);
        std::filesystem::create_directory(outsidePath);
        auto opened = vtpty::nativeDirectoryOpener().open(pathToUtf8(rootPath));
        REQUIRE(opened.has_value());
        return std::move(*opened);
    }
};

/// @return @p name opened beneath @p parent as @p entry, by this machine's calls.
[[nodiscard]] std::expected<vtpty::UniqueHandle, DWORD> openIn(HANDLE parent,
                                                               std::wstring_view name,
                                                               BeneathEntry entry = BeneathEntry::Entry)
{
    return vtpty::nativeWin32BeneathSystem().open(parent, name, entry);
}

/// @return What @p handle is, by this machine's calls; the test fails if it cannot be told.
[[nodiscard]] vtpty::FileStatus statusOf(HANDLE handle)
{
    auto status = vtpty::nativeWin32BeneathSystem().status(handle);
    REQUIRE(status.has_value());
    return *status;
}

/// @return Whether @p handle would be inherited by a child process.
[[nodiscard]] bool isInheritable(HANDLE handle)
{
    auto flags = DWORD { 0 };
    REQUIRE(GetHandleInformation(handle, &flags) != FALSE);
    return (flags & HANDLE_FLAG_INHERIT) != 0;
}

} // namespace

TEST_CASE("Win32BeneathSystem: a file is regular, with one link, on the root's volume", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    writeFile(fixture.rootPath / "f", "abc");

    auto const file = openIn(fixture.root.nativeHandle(), L"f");
    REQUIRE(file.has_value());
    auto const status = statusOf(file->get());
    CHECK(status.kind == vtpty::FileKind::Regular);
    CHECK(status.links == 1);
    CHECK(status.sizeBytes == 3);
    CHECK(status.identity.device == statusOf(fixture.root.nativeHandle()).identity.device);
    CHECK(status.identity.inode != 0);

    auto const nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count());
    auto constexpr OneMinuteMs = uint64_t { 60'000 };
    CHECK(status.modifiedUnixMs + OneMinuteMs > nowMs);
    CHECK(status.modifiedUnixMs < nowMs + OneMinuteMs);
}

TEST_CASE("Win32BeneathSystem: one file opened by two spellings is one identity", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    writeFile(fixture.rootPath / "f", "abc");
    writeFile(fixture.rootPath / "g", "abc");

    auto const lower = openIn(fixture.root.nativeHandle(), L"f");
    auto const upper = openIn(fixture.root.nativeHandle(), L"F");
    auto const other = openIn(fixture.root.nativeHandle(), L"g");
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());
    REQUIRE(other.has_value());
    CHECK(statusOf(lower->get()).identity == statusOf(upper->get()).identity);
    CHECK(statusOf(lower->get()).identity != statusOf(other->get()).identity);
}

TEST_CASE("Win32BeneathSystem: a junction is opened itself, as a link", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    REQUIRE(vtpty::testing::createJunction(fixture.rootPath / "j", fixture.outsidePath).has_value());

    auto const asDirectory = openIn(fixture.root.nativeHandle(), L"j", BeneathEntry::Directory);
    REQUIRE(asDirectory.has_value());
    CHECK(statusOf(asDirectory->get()).kind == vtpty::FileKind::Symlink);

    auto const asEntry = openIn(fixture.root.nativeHandle(), L"j", BeneathEntry::Entry);
    REQUIRE(asEntry.has_value());
    CHECK(statusOf(asEntry->get()).kind == vtpty::FileKind::Symlink);
}

TEST_CASE("Win32BeneathSystem: a directory open of a file is ERROR_DIRECTORY", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    writeFile(fixture.rootPath / "f", "abc");

    auto const opened = openIn(fixture.root.nativeHandle(), L"f", BeneathEntry::Directory);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error() == DWORD { ERROR_DIRECTORY });
}

TEST_CASE("Win32BeneathSystem: a missing entry is ERROR_FILE_NOT_FOUND", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    for (auto const entry: { BeneathEntry::Directory, BeneathEntry::Entry })
    {
        auto const opened = openIn(fixture.root.nativeHandle(), L"missing", entry);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == DWORD { ERROR_FILE_NOT_FOUND });
    }
}

TEST_CASE("Win32BeneathSystem: a hard link counts its names", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    auto const target = fixture.outsidePath / "private";
    writeFile(target, "secret");
    REQUIRE(CreateHardLinkW((fixture.rootPath / "h").c_str(), target.c_str(), nullptr) != FALSE);

    auto const link = openIn(fixture.root.nativeHandle(), L"h");
    REQUIRE(link.has_value());
    CHECK(statusOf(link->get()).links == 2);
}

TEST_CASE("Win32BeneathSystem: a removed held directory has no links", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    auto const path = fixture.rootPath / "wt";
    std::filesystem::create_directory(path);
    auto const held = vtpty::nativeDirectoryOpener().open(pathToUtf8(path));
    REQUIRE(held.has_value());
    REQUIRE(std::filesystem::remove(path));

    CHECK(statusOf(held->nativeHandle()).links == 0);
}

TEST_CASE("Win32BeneathSystem: a re-opened file reads its bytes, and is the same file", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    writeFile(fixture.rootPath / "f", "abc");
    auto& system = vtpty::nativeWin32BeneathSystem();

    auto const looked = openIn(fixture.root.nativeHandle(), L"f");
    REQUIRE(looked.has_value());
    auto const data = system.reopenForRead(looked->get());
    REQUIRE(data.has_value());
    CHECK(statusOf(data->get()).identity == statusOf(looked->get()).identity);

    auto buffer = std::array<char, 16> {};
    auto const first = system.read(data->get(), buffer);
    REQUIRE(first.has_value());
    REQUIRE(*first == 3);
    CHECK(std::string_view { buffer.data(), *first } == "abc");
    auto const second = system.read(data->get(), buffer);
    REQUIRE(second.has_value());
    CHECK(*second == 0);

    // Shared for deletion: reading never stops the file from being removed.
    CHECK(std::filesystem::remove(fixture.rootPath / "f"));
}

TEST_CASE("Win32BeneathSystem: nothing is re-opened without a handle", "[vtpty][beneath]")
{
    auto const unheld = vtpty::nativeWin32BeneathSystem().reopenForRead(nullptr);
    REQUIRE_FALSE(unheld.has_value());
    CHECK(unheld.error() == DWORD { ERROR_ACCESS_DENIED });
}

TEST_CASE("Win32BeneathSystem: a directory is never re-opened for reading", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    std::filesystem::create_directory(fixture.rootPath / "d");
    auto& system = vtpty::nativeWin32BeneathSystem();

    auto const looked = openIn(fixture.root.nativeHandle(), L"d");
    REQUIRE(looked.has_value());
    CHECK_FALSE(system.reopenForRead(looked->get()).has_value());
    CHECK_FALSE(system.reopenForRead(fixture.root.nativeHandle()).has_value());
}

TEST_CASE("Win32BeneathSystem: no handle it makes is inheritable", "[vtpty][beneath]")
{
    auto const fixture = Fixture {};
    writeFile(fixture.rootPath / "f", "abc");
    std::filesystem::create_directory(fixture.rootPath / "d");

    auto const directory = openIn(fixture.root.nativeHandle(), L"d", BeneathEntry::Directory);
    auto const looked = openIn(fixture.root.nativeHandle(), L"f");
    REQUIRE(directory.has_value());
    REQUIRE(looked.has_value());
    auto const data = vtpty::nativeWin32BeneathSystem().reopenForRead(looked->get());
    REQUIRE(data.has_value());

    CHECK_FALSE(isInheritable(directory->get()));
    CHECK_FALSE(isInheritable(looked->get()));
    CHECK_FALSE(isInheritable(data->get()));
}

#endif
