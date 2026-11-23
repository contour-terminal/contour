// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
    #include <sys/stat.h>
#else
    #include <core/Utf8Path.hpp>

    #include <Windows.h>
    #include <winioctl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

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

#endif

#ifdef _WIN32

namespace
{

using core::pathToUtf8;

/// The layout of `REPARSE_DATA_BUFFER::MountPointReparseBuffer`, which `<Windows.h>` does not declare: the
/// header of a junction's reparse data, followed by its two NUL-terminated names.
struct MountPointHeader
{
    DWORD reparseTag;
    WORD reparseDataLength;
    WORD reserved;
    WORD substituteNameOffset;
    WORD substituteNameLength;
    WORD printNameOffset;
    WORD printNameLength;
};

/// Makes @p link a junction to the directory @p target: a redirection any user may create, unlike a symlink.
/// @return Whether it was made.
[[nodiscard]] bool createJunction(std::filesystem::path const& link, std::filesystem::path const& target)
{
    if (CreateDirectoryW(link.c_str(), nullptr) == FALSE)
        return false;
    auto const handle = CreateFileW(link.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    auto const printName = std::filesystem::absolute(target).native();
    auto const substituteName = L"\\??\\" + printName;
    auto const substituteBytes = substituteName.size() * sizeof(wchar_t);
    auto const printBytes = printName.size() * sizeof(wchar_t);
    auto const namesBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    auto const header = MountPointHeader {
        .reparseTag = IO_REPARSE_TAG_MOUNT_POINT,
        .reparseDataLength = static_cast<WORD>(
            sizeof(MountPointHeader) - offsetof(MountPointHeader, substituteNameOffset) + namesBytes),
        .reserved = 0,
        .substituteNameOffset = 0,
        .substituteNameLength = static_cast<WORD>(substituteBytes),
        .printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t)),
        .printNameLength = static_cast<WORD>(printBytes),
    };
    auto buffer = std::vector<std::byte>(sizeof(MountPointHeader) + namesBytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substituteName.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) + header.printNameOffset,
                printName.c_str(),
                printBytes + sizeof(wchar_t));

    auto returned = DWORD { 0 };
    auto const set = DeviceIoControl(handle,
                                     FSCTL_SET_REPARSE_POINT,
                                     buffer.data(),
                                     static_cast<DWORD>(buffer.size()),
                                     nullptr,
                                     0,
                                     &returned,
                                     nullptr);
    CloseHandle(handle);
    return set != FALSE;
}

/// @return @p p's canonical path, in UTF-8: what a directory handle's resolvedPath() is compared with.
[[nodiscard]] std::string canonicalOf(std::filesystem::path const& p)
{
    return pathToUtf8(std::filesystem::canonical(p));
}

/// @return @p text with its ASCII letters upper-cased.
[[nodiscard]] std::string upperCased(std::string text)
{
    std::ranges::transform(text, text.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    return text;
}

} // namespace

TEST_CASE("DirectoryHandle: open reads the canonical path from the handle", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const real = directory.path() / "real";
    std::filesystem::create_directory(real);
    auto& opener = vtpty::nativeDirectoryOpener();

    // Spelled with forward slashes, and in another case: the handle tells the true spelling.
    auto spelled = upperCased(pathToUtf8(real));
    std::ranges::replace(spelled, '\\', '/');
    auto const opened = opener.open(spelled);
    REQUIRE(opened.has_value());
    CHECK(opened->nativeHandle() != nullptr);
    CHECK(opened->resolvedPath() == canonicalOf(real));
    CHECK_FALSE(opened->resolvedPath().starts_with(R"(\\?\)"));

    auto const junction = directory.path() / "j";
    REQUIRE(createJunction(junction, real));
    auto const viaJunction = opener.open(pathToUtf8(junction));
    REQUIRE(viaJunction.has_value());
    CHECK(viaJunction->resolvedPath() == canonicalOf(real));
    CHECK(opener.canonical(pathToUtf8(junction)) == canonicalOf(real));

    auto const unavailable =
        vtpty::makeDirectoryOpener(vtpty::DescriptorPaths::Unavailable)->open(pathToUtf8(real));
    REQUIRE(unavailable.has_value());
    CHECK(unavailable->resolvedPath() == canonicalOf(real));
}

TEST_CASE("DirectoryHandle: open follows a directory symlink", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const real = directory.path() / "real";
    std::filesystem::create_directory(real);
    auto const link = directory.path() / "link";
    auto error = std::error_code {};
    std::filesystem::create_directory_symlink(real, link, error);
    if (error)
        SKIP("no symlink privilege");

    auto const opened = vtpty::nativeDirectoryOpener().open(pathToUtf8(link));
    REQUIRE(opened.has_value());
    CHECK(opened->resolvedPath() == canonicalOf(real));
}

TEST_CASE("DirectoryHandle: canonical resolves a file, and a missing path is Missing", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const& dir = directory.path();
    auto const file = dir / "file";
    std::ofstream { file } << "x";
    auto& opener = vtpty::nativeDirectoryOpener();

    CHECK(opener.canonical(pathToUtf8(file)) == canonicalOf(file));
    CHECK(opener.canonical(pathToUtf8(dir / "missing")).error() == vtpty::DirectoryError::Missing);
}

TEST_CASE("DirectoryHandle: a held directory may be renamed and removed, and tells it was removed",
          "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const a = directory.path() / "a";
    auto const aOld = directory.path() / "a.old";
    std::filesystem::create_directory(a);
    auto const held = vtpty::nativeDirectoryOpener().open(pathToUtf8(a));
    REQUIRE(held.has_value());

    CHECK_NOTHROW(std::filesystem::rename(a, aOld));
    CHECK_FALSE(held->isRemoved());
    CHECK(std::filesystem::remove(aOld));
    CHECK(held->isRemoved());
}

TEST_CASE("DirectoryHandle: a path longer than MAX_PATH", "[processrunner]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-directoryhandle" };
    auto const component = std::wstring(60, L'c');
    // The canonical spelling, so that a short (8.3) name in the temporary directory does not differ.
    auto const base = std::filesystem::canonical(directory.path());
    auto longPath = base;
    for ([[maybe_unused]] auto const level: std::views::iota(0, 6))
    {
        longPath /= component;
        auto const verbatim = L"\\\\?\\" + longPath.native();
        REQUIRE(CreateDirectoryW(verbatim.c_str(), nullptr) != FALSE);
    }
    REQUIRE(longPath.native().size() > MAX_PATH);

    {
        auto const opened = vtpty::nativeDirectoryOpener().open(pathToUtf8(longPath));
        REQUIRE(opened.has_value());
        CHECK(opened->resolvedPath() == pathToUtf8(longPath));
    }

    // ScopedTempDir removes by the plain spelling, which cannot reach this deep.
    auto error = std::error_code {};
    std::filesystem::remove_all(std::filesystem::path { L"\\\\?\\" + (base / component).native() }, error);
}

TEST_CASE("DirectoryHandle: an unbacked handle holds no HANDLE", "[processrunner]")
{
    auto const handle = vtpty::DirectoryHandle::unbacked(R"(C:\somewhere)");
    CHECK(handle.nativeHandle() == nullptr);
    CHECK_FALSE(handle.isRemoved());
}

#endif
