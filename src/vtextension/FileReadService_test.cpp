// SPDX-License-Identifier: Apache-2.0
//
// The policy of `file-read`: which roots may be read, which paths are refused before anything is opened, how
// a root's directory is opened and judged, and how every answer of the read beneath it is worded.
#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>
#include <vtpty/PathStyle.hpp>
#ifdef _WIN32
    #include <vtpty/testing/Junction.hpp>
    #include <vtpty/testing/LegacyRemove.hpp>
    #include <vtpty/testing/ShortPath.hpp>

    #include <Windows.h>
#endif

#include <core/Utf8Path.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/FileReadService.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/testing/FakeBeneathReader.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>

using namespace std::string_view_literals;
using vtextension::FileContent;
using vtextension::FileError;
using vtextension::FileReadService;
using vtextension::ResolvedRoot;
using vtpty::BeneathReadError;

namespace
{

/// @return @p text as the world's bytes.
[[nodiscard]] vtextension::ByteList bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

/// An extension that declares `read worktrees`, and `repos` beside it.
struct Fixture
{
    explicit Fixture(std::vector<ResolvedRoot> resolved = { { .label = "worktrees", .paths = { "/wt" } },
                                                            { .label = "repos", .paths = { "/src/repo" } } }):
        Fixture { vtpty::PathStyle::Posix, std::move(resolved) }
    {
    }

    /// @param style The path grammar the directory opener and the reader speak.
    /// @param resolved The roots, as resolved in @p style.
    Fixture(vtpty::PathStyle style, std::vector<ResolvedRoot> resolved):
        roots { std::move(resolved) }, directories { style }, reader { style }
    {
        manifest.name = "orchestrator";
        manifest.readableRoots = { "worktrees" };
        for (auto const& root: roots)
            for (auto const& path: root.paths)
                directories.add(path);
        service.emplace(manifest, roots, directories, reader);
    }

    [[nodiscard]] std::expected<FileContent, FileError> read(std::string_view root, std::string_view path)
    {
        return service->read(root, path);
    }

    vtextension::Manifest manifest;
    std::vector<ResolvedRoot> roots;
    vtextension::testing::FakeDirectoryOpener directories;
    vtextension::testing::FakeBeneathReader reader;
    std::optional<FileReadService> service;
};

} // namespace

TEST_CASE("FileReadService: a file beneath a readable root is read", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.reader.add("/wt/run-1/.git", "gitdir: /src/repo/.git/worktrees/run-1\n", 1'700'000'000'123U);

    auto const content = fixture.read("worktrees", "run-1/.git");
    REQUIRE(content.has_value());
    CHECK(content->bytes == bytesOf("gitdir: /src/repo/.git/worktrees/run-1\n"));
    CHECK(content->modifiedUnixMs == 1'700'000'000'123U);
    CHECK(fixture.reader.reads
          == std::vector { std::pair { std::string { "/wt/run-1/.git" }, vtextension::MaxFileReadBytes } });
}

TEST_CASE("FileReadService: a path under repos is not-granted", "[vtextension][file-read]")
{
    SECTION("a root not declared readable")
    {
        auto fixture = Fixture {};
        fixture.reader.add("/src/repo/.git/config", "[core]\n");
        CHECK(fixture.read("repos", ".git/config") == std::unexpected(FileError::NotGranted));
        CHECK(fixture.read("nowhere", "x") == std::unexpected(FileError::NotGranted));
        CHECK(fixture.reader.reads.empty());
    }
    SECTION("repos nested inside the readable root")
    {
        auto fixture = Fixture { { { .label = "worktrees", .paths = { "/wt" } },
                                   { .label = "repos", .paths = { "/wt/main" } } } };
        fixture.reader.add("/wt/main/.git/config", "[core]\n");
        CHECK(fixture.read("worktrees", "main/.git/config") == std::unexpected(FileError::NotGranted));
        CHECK(fixture.read("worktrees", "main") == std::unexpected(FileError::NotGranted));
        CHECK(fixture.reader.reads.empty());
    }
    SECTION("repos nested inside the readable root, reached by another spelling")
    {
        // A case-insensitive filesystem: "main" is the directory /wt/Main, which is the repos root.
        auto fixture = Fixture { { { .label = "worktrees", .paths = { "/wt" } },
                                   { .label = "repos", .paths = { "/wt/Main" } } } };
        fixture.reader.identities.emplace("/wt/Main", vtpty::FileIdentity { .device = 1, .inode = 7 });
        fixture.reader.identities.emplace("/wt/main", vtpty::FileIdentity { .device = 1, .inode = 7 });
        fixture.reader.identities.emplace("/wt/other", vtpty::FileIdentity { .device = 1, .inode = 8 });
        fixture.reader.add("/wt/main/.git/config", "[core]\n");
        fixture.reader.add("/wt/other/.git/config", "[core]\n");
        CHECK(fixture.read("worktrees", "main/.git/config") == std::unexpected(FileError::NotGranted));
        CHECK(fixture.read("worktrees", "other/.git/config").has_value());
    }
}

TEST_CASE("FileReadService: bad paths", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    for (auto const path: std::array {
             ""sv, "/abs"sv, "/wt/a"sv, "a/../b"sv, ".."sv, "a//b"sv, "./a"sv, "a/."sv, "a/"sv, "a\0b"sv })
    {
        CAPTURE(path);
        CHECK(fixture.read("worktrees", path) == std::unexpected(FileError::BadArgument));
    }
    CHECK(fixture.reader.reads.empty());
}

TEST_CASE("FileReadService: the root's directory gone is directory-gone", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.directories.directories.erase("/wt");
    CHECK(fixture.read("worktrees", "run-1/.git") == std::unexpected(FileError::DirectoryGone));
    CHECK(fixture.reader.reads.empty());
}

TEST_CASE("FileReadService: a root that now resolves elsewhere escapes", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.directories.link("/wt", "/home/me/.ssh");
    fixture.reader.add("/home/me/.ssh/id_ed25519", "private");
    CHECK(fixture.read("worktrees", "id_ed25519") == std::unexpected(FileError::Escapes));
    CHECK(fixture.reader.reads.empty());
}

TEST_CASE("FileReadService: a root's directory that cannot be opened", "[vtextension][file-read]")
{
    struct Row
    {
        vtpty::DirectoryError opened;
        FileError answer;
    };
    for (auto const row: std::array {
             Row { .opened = vtpty::DirectoryError::NotADirectory, .answer = FileError::DirectoryGone },
             Row { .opened = vtpty::DirectoryError::AccessDenied, .answer = FileError::Unreadable },
             Row { .opened = vtpty::DirectoryError::Unsupported, .answer = FileError::NotSupportedYet },
         })
    {
        CAPTURE(row.opened);
        auto fixture = Fixture {};
        fixture.directories.failures.emplace("/wt", row.opened);
        CHECK(fixture.read("worktrees", "a") == std::unexpected(row.answer));
    }
}

TEST_CASE("FileReadService: reader errors map", "[vtextension][file-read]")
{
    struct Row
    {
        BeneathReadError read;
        FileError answer;
    };
    for (auto const row: std::array {
             Row { .read = BeneathReadError::DirectoryMissing, .answer = FileError::DirectoryGone },
             Row { .read = BeneathReadError::NotRegular, .answer = FileError::NotRegular },
             Row { .read = BeneathReadError::Unreadable, .answer = FileError::Unreadable },
             Row { .read = BeneathReadError::TooLarge, .answer = FileError::TooLarge },
             Row { .read = BeneathReadError::Escapes, .answer = FileError::Escapes },
             Row { .read = BeneathReadError::Unsupported, .answer = FileError::NotSupportedYet },
             Row { .read = BeneathReadError::Fenced, .answer = FileError::NotGranted },
         })
    {
        CAPTURE(row.read);
        auto fixture = Fixture {};
        fixture.reader.otherwise = row.read;
        CHECK(fixture.read("worktrees", "run-1/.git") == std::unexpected(row.answer));
    }
}

TEST_CASE("FileReadService: a root of several directories is read in declaration order",
          "[vtextension][file-read]")
{
    auto fixture = Fixture { { { .label = "worktrees", .paths = { "/a", "/b", "/c" } } } };
    fixture.directories.directories.erase("/a");
    fixture.reader.files.emplace("/b/run/.git", std::unexpected(BeneathReadError::DirectoryMissing));
    fixture.reader.add("/c/run/.git", "found");

    auto const content = fixture.read("worktrees", "run/.git");
    REQUIRE(content.has_value());
    CHECK(content->bytes == bytesOf("found"));

    // A file missing in an existing directory answers there: the next directory is not asked.
    fixture.reader.files.insert_or_assign("/b/run/.git", std::unexpected(BeneathReadError::NotRegular));
    CHECK(fixture.read("worktrees", "run/.git") == std::unexpected(FileError::NotRegular));

    auto empty = Fixture { { { .label = "worktrees", .paths = {} } } };
    CHECK(empty.read("worktrees", "a") == std::unexpected(FileError::DirectoryGone));
}

TEST_CASE("FileReadService: a root's directory is opened once across reads", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.reader.add("/wt/run-1/.git", "gitdir: x\n");
    fixture.reader.files.emplace("/wt/gone/.git", std::unexpected(BeneathReadError::DirectoryMissing));
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
    {
        CHECK(fixture.read("worktrees", "run-1/.git").has_value());
        // A removed worktree below the root is not the root removed: nothing is opened again for it.
        CHECK(fixture.read("worktrees", "gone/.git") == std::unexpected(FileError::DirectoryGone));
    }
    CHECK(fixture.directories.opened == std::vector<std::string> { "/wt" });
}

TEST_CASE("FileReadService: a fence found once holds after the nested root is renamed away and back",
          "[vtextension][file-read]")
{
    auto fixture = Fixture { { { .label = "worktrees", .paths = { "/wt" } },
                               { .label = "repos", .paths = { "/wt/main" } } } };
    fixture.reader.identities.emplace("/wt/main", vtpty::FileIdentity { .device = 1, .inode = 7 });
    fixture.reader.add("/wt/run-1/.git", "gitdir: x\n");
    REQUIRE(fixture.read("worktrees", "run-1/.git").has_value());

    // Renamed away, then back under another spelling a case-insensitive filesystem takes for the same name:
    // the identity found on the first read still fences it.
    fixture.reader.identities.erase("/wt/main");
    fixture.reader.identities.emplace("/wt/Main", vtpty::FileIdentity { .device = 1, .inode = 7 });
    fixture.reader.add("/wt/Main/.git/config", "[core]\n");
    CHECK(fixture.read("worktrees", "Main/.git/config") == std::unexpected(FileError::NotGranted));
    CHECK(fixture.directories.opened == std::vector<std::string> { "/wt" });
}

TEST_CASE("FileReadService: a removed and recreated root is opened anew", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.reader.add("/wt/run-1/.git", "gitdir: x\n");
    REQUIRE(fixture.read("worktrees", "run-1/.git").has_value());

    fixture.reader.removedRoots.insert("/wt");
    fixture.directories.onOpen = [&](std::string_view /*path*/) {
        fixture.reader.removedRoots.clear();
    };
    CHECK(fixture.read("worktrees", "run-1/.git").has_value());
    CHECK(fixture.directories.opened == std::vector<std::string> { "/wt", "/wt" });
}

TEST_CASE("FileReadService: a root that did not exist is opened once it does", "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.directories.directories.erase("/wt");
    fixture.reader.add("/wt/run-1/.git", "gitdir: x\n");
    CHECK(fixture.read("worktrees", "run-1/.git") == std::unexpected(FileError::DirectoryGone));
    fixture.directories.add("/wt");
    CHECK(fixture.read("worktrees", "run-1/.git").has_value());
    CHECK(fixture.directories.opened == std::vector<std::string> { "/wt", "/wt" });
}

TEST_CASE("FileReadService: a root removed while held is directory-gone until it is back",
          "[vtextension][file-read]")
{
    auto fixture = Fixture {};
    fixture.reader.add("/wt/run-1/.git", "gitdir: x\n");
    REQUIRE(fixture.read("worktrees", "run-1/.git").has_value());

    // Removed, and not there when it is opened again.
    fixture.reader.removedRoots.insert("/wt");
    fixture.directories.directories.erase("/wt");
    CHECK(fixture.read("worktrees", "run-1/.git") == std::unexpected(FileError::DirectoryGone));

    // Back: opened anew on the next read.
    fixture.reader.removedRoots.clear();
    fixture.directories.add("/wt");
    CHECK(fixture.read("worktrees", "run-1/.git").has_value());
    CHECK(fixture.directories.opened == std::vector<std::string> { "/wt", "/wt", "/wt" });
}

namespace
{

/// An extension that declares `read worktrees`, and `repos` beside it, on Windows.
[[nodiscard]] Fixture windowsFixture()
{
    return Fixture { vtpty::PathStyle::Windows,
                     { { .label = "worktrees", .paths = { R"(C:\wt)" } },
                       { .label = "repos", .paths = { R"(C:\src\repo)" } } } };
}

} // namespace

TEST_CASE("FileReadService: on Windows a component Windows spells otherwise is a bad argument",
          "[vtextension][file-read]")
{
    auto fixture = windowsFixture();
    fixture.reader.otherwise = BeneathReadError::NotRegular;
    for (auto const path:
         std::array { R"(a\..\..\x)"sv, "STATUS.md:x"sv, "con"sv, "x."sv, "x "sv, R"(a/b\c)"sv, R"(a\b)"sv })
    {
        CAPTURE(path);
        CHECK(fixture.read("worktrees", path) == std::unexpected(FileError::BadArgument));
    }
    CHECK(fixture.reader.reads.empty());

    // A POSIX name may hold a backslash: there it is one more byte of the name.
    auto posix = Fixture {};
    posix.reader.add(R"(/wt/a\b)", "plain");
    auto const content = posix.read("worktrees", R"(a\b)");
    REQUIRE(content.has_value());
    CHECK(content->bytes == bytesOf("plain"));
}

TEST_CASE("FileReadService: a Windows file beneath a readable root is read", "[vtextension][file-read]")
{
    auto fixture = windowsFixture();
    fixture.reader.add(R"(C:\wt\contour\r1\STATUS.md)", "# r1\n");

    auto const content = fixture.read("worktrees", "contour/r1/STATUS.md");
    REQUIRE(content.has_value());
    CHECK(content->bytes == bytesOf("# r1\n"));
    CHECK(fixture.reader.reads
          == std::vector {
              std::pair { std::string { R"(C:\wt\contour\r1\STATUS.md)" }, vtextension::MaxFileReadBytes } });
}

TEST_CASE("FileReadService: a Windows root read back in another case is the same directory",
          "[vtextension][file-read]")
{
    SECTION("the same directory, in the case it has on disk")
    {
        auto fixture = windowsFixture();
        fixture.directories.link(R"(C:\wt)", R"(C:\WT)");
        fixture.reader.add(R"(C:\WT\x)", "read");
        auto const content = fixture.read("worktrees", "x");
        REQUIRE(content.has_value());
        CHECK(content->bytes == bytesOf("read"));
    }
    SECTION("another drive")
    {
        auto fixture = windowsFixture();
        fixture.directories.link(R"(C:\wt)", R"(D:\wt)");
        fixture.reader.add(R"(D:\wt\x)", "elsewhere");
        CHECK(fixture.read("worktrees", "x") == std::unexpected(FileError::Escapes));
        CHECK(fixture.reader.reads.empty());
    }
}

TEST_CASE("FileReadService: a Windows nested root in another case is still fenced",
          "[vtextension][file-read]")
{
    auto fixture = Fixture { vtpty::PathStyle::Windows,
                             { { .label = "worktrees", .paths = { R"(C:\wt)" } },
                               { .label = "repos", .paths = { R"(C:\WT\inner)" } } } };
    fixture.reader.identities.emplace(R"(C:\wt\inner)", vtpty::FileIdentity { .device = 1, .inode = 7 });
    fixture.reader.add(R"(C:\wt\inner\x)", "fenced");
    fixture.reader.add(R"(C:\wt\run-1\x)", "beside");

    CHECK(fixture.read("worktrees", "inner/x") == std::unexpected(FileError::NotGranted));
    CHECK(fixture.read("worktrees", "run-1/x").has_value());
    // The walk is handed the nested root by its components beneath this one, whatever their case.
    CHECK(std::ranges::contains(fixture.reader.identified, std::string { R"(C:\wt\inner)" }));
}

TEST_CASE("FileReadService: a nested root's other name is fenced by its identity alone",
          "[vtextension][file-read]")
{
    // `INNER-~1` is an 8.3 name of the nested root: no spelling rule ties it to `inner-repository`, so the
    // path check passes it, and only the identity the walk meets refuses it.
    auto fixture = Fixture { vtpty::PathStyle::Windows,
                             { { .label = "worktrees", .paths = { R"(C:\wt)" } },
                               { .label = "repos", .paths = { R"(C:\wt\inner-repository)" } } } };
    auto constexpr Nested = vtpty::FileIdentity { .device = 1, .inode = 7 };
    fixture.reader.identities.emplace(R"(C:\wt\inner-repository)", Nested);
    fixture.reader.identities.emplace(R"(C:\wt\INNER-~1)", Nested);
    fixture.reader.identities.emplace(R"(C:\wt\run-1)", vtpty::FileIdentity { .device = 1, .inode = 8 });
    fixture.reader.add(R"(C:\wt\INNER-~1\x)", "fenced");
    fixture.reader.add(R"(C:\wt\run-1\x)", "beside");

    CHECK(fixture.read("worktrees", "inner-repository/x") == std::unexpected(FileError::NotGranted));
    CHECK(fixture.reader.reads.empty());
    CHECK(fixture.read("worktrees", "INNER-~1/x") == std::unexpected(FileError::NotGranted));
    CHECK(fixture.read("worktrees", "run-1/x").has_value());
}

namespace
{

/// This machine's directories, recording every path it is asked to open.
class RecordingOpener final: public vtpty::IDirectoryOpener
{
  public:
    [[nodiscard]] std::expected<vtpty::DirectoryHandle, vtpty::DirectoryError> open(
        std::string_view path) override
    {
        opened.emplace_back(path);
        if (beforeOpen)
            beforeOpen();
        auto handle = vtpty::nativeDirectoryOpener().open(path);
        if (handle && onOpened)
            onOpened(*handle);
        return handle;
    }

    [[nodiscard]] std::expected<std::string, vtpty::DirectoryError> canonical(
        std::string_view existingPath) override
    {
        return vtpty::nativeDirectoryOpener().canonical(existingPath);
    }

    [[nodiscard]] std::expected<vtpty::DirectoryHandle, vtpty::DirectoryError> createBeneath(
        vtpty::DirectoryHandle const& parent, std::string_view name) override
    {
        return vtpty::nativeDirectoryOpener().createBeneath(parent, name);
    }

    [[nodiscard]] vtpty::PathStyle pathStyle() const noexcept override
    {
        return vtpty::nativeDirectoryOpener().pathStyle();
    }

    std::vector<std::string> opened; ///< Every path open() was asked for, in order.

    std::function<void()> beforeOpen;                                   ///< Runs before every open.
    std::function<void(vtpty::DirectoryHandle const& handle)> onOpened; ///< Runs on every directory opened.
};

} // namespace

#ifndef _WIN32

namespace
{

/// This machine's system calls, but a mount over every name in @p mounted: the kernel refuses to cross it.
class MountedSystem final: public vtpty::IBeneathSystem
{
  public:
    explicit MountedSystem(std::vector<std::string> mounted): _mounted { std::move(mounted) } {}

    [[nodiscard]] std::expected<vtpty::OwnedFd, int> open(int directory,
                                                          std::string const& name,
                                                          vtpty::BeneathTarget target) override
    {
        if (std::ranges::contains(_mounted, name))
            return std::unexpected(EXDEV);
        return vtpty::nativeBeneathSystem().open(directory, name, target);
    }

    [[nodiscard]] std::expected<vtpty::FileStatus, int> statusAt(int directory,
                                                                 std::string const& name) override
    {
        if (std::ranges::contains(_mounted, name))
            return std::unexpected(EXDEV);
        return vtpty::nativeBeneathSystem().statusAt(directory, name);
    }

    [[nodiscard]] std::expected<vtpty::FileStatus, int> status(int descriptor) override
    {
        return vtpty::nativeBeneathSystem().status(descriptor);
    }

  private:
    std::vector<std::string> _mounted;
};

} // namespace

TEST_CASE("FileReadService: a nested root behind a mount is never asked", "[vtextension][file-read]")
{
    // /wt is readable; /wt/main is the repos root, with a (stalled, say) mount over it. Neither fencing it
    // nor reading beside it may ask the mount anything; reading into it is refused at the crossing.
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-mount" };
    auto const wt = std::filesystem::canonical(temp.path());
    std::filesystem::create_directories(wt / "main" / ".git");
    std::filesystem::create_directories(wt / "run-1");
    std::ofstream { wt / "run-1" / ".git" } << "gitdir: x\n";
    std::ofstream { wt / "main" / ".git" / "config" } << "[core]\n";

    auto manifest = vtextension::Manifest {};
    manifest.name = "orchestrator";
    manifest.readableRoots = { "worktrees" };
    auto system = MountedSystem { { "main" } };
    auto const reader = vtpty::makeBeneathReader(system);
    auto opener = RecordingOpener {};
    auto service = FileReadService { manifest,
                                     { { .label = "worktrees", .paths = { wt.string() } },
                                       { .label = "repos", .paths = { (wt / "main").string() } } },
                                     opener,
                                     *reader };

    auto const sibling = service.read("worktrees", "run-1/.git");
    REQUIRE(sibling.has_value());
    CHECK(sibling->bytes == bytesOf("gitdir: x\n"));
    CHECK(service.read("worktrees", "main/.git/config") == std::unexpected(FileError::NotGranted));
    // Only the readable root's own directory was opened by path; the nested root never was.
    CHECK(std::ranges::all_of(opener.opened, [&](std::string const& path) { return path == wt.string(); }));
}

TEST_CASE("FileReadService: a nested root is fenced through the walk, not by path",
          "[vtextension][file-read]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-fence" };
    auto const wt = std::filesystem::canonical(temp.path());
    std::filesystem::create_directories(wt / "Main" / ".git");
    std::ofstream { wt / "Main" / ".git" / "config" } << "[core]\n";

    auto manifest = vtextension::Manifest {};
    manifest.name = "orchestrator";
    manifest.readableRoots = { "worktrees" };
    auto opener = RecordingOpener {};
    auto service = FileReadService { manifest,
                                     { { .label = "worktrees", .paths = { wt.string() } },
                                       { .label = "repos", .paths = { (wt / "Main").string() } } },
                                     opener,
                                     vtpty::nativeBeneathReader() };
    CHECK(service.read("worktrees", "Main/.git/config") == std::unexpected(FileError::NotGranted));
    CHECK(std::ranges::all_of(opener.opened, [&](std::string const& path) { return path == wt.string(); }));
}

#endif

#ifdef _WIN32

namespace
{

/// An extension that declares `read worktrees` and nothing else.
[[nodiscard]] vtextension::Manifest readingWorktrees()
{
    auto manifest = vtextension::Manifest {};
    manifest.name = "orchestrator";
    manifest.readableRoots = { "worktrees" };
    return manifest;
}

/// Writes @p text to @p file, byte for byte.
void writeFile(std::filesystem::path const& file, std::string_view text)
{
    auto out = std::ofstream { file, std::ios::binary };
    out << text;
}

} // namespace

TEST_CASE("FileReadService: a Windows worktree's STATUS.md is read through the native reader",
          "[vtextension][file-read]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-status" };
    auto const wt = std::filesystem::canonical(temp.path());
    std::filesystem::create_directories(wt / "contour" / "r1");
    writeFile(wt / "contour" / "r1" / "STATUS.md", "# Board\n");

    auto opener = RecordingOpener {};
    auto service = FileReadService { readingWorktrees(),
                                     { { .label = "worktrees", .paths = { core::pathToUtf8(wt) } } },
                                     opener,
                                     vtpty::nativeBeneathReader() };
    auto const content = service.read("worktrees", "contour/r1/STATUS.md");
    REQUIRE(content.has_value());
    CHECK(content->bytes == bytesOf("# Board\n"));
    CHECK(content->modifiedUnixMs != 0);
    // Only the root is opened by path; everything beneath it is reached from its handle.
    CHECK(opener.opened == std::vector { core::pathToUtf8(wt) });
}

TEST_CASE("FileReadService: a nested root is fenced through the walk, not by path, on Windows",
          "[vtextension][file-read]")
{
    // The nested root reached by its 8.3 name: no spelling rule ties `MAIN-L~1` to `Main-long-name`, so the
    // path check passes it, and only the identity the walk meets refuses it.
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-fence" };
    auto const wt = std::filesystem::canonical(temp.path());
    auto const nested = wt / "Main-long-name";
    std::filesystem::create_directories(nested / ".git");
    writeFile(nested / ".git" / "config", "[core]\n");
    auto const shortName = core::pathToUtf8(vtpty::testing::shortPathOf(nested).filename());
    if (!shortName.contains('~'))
    {
        WARN("no 8.3 names on this volume");
        return;
    }

    auto opener = RecordingOpener {};
    auto service = FileReadService { readingWorktrees(),
                                     { { .label = "worktrees", .paths = { core::pathToUtf8(wt) } },
                                       { .label = "repos", .paths = { core::pathToUtf8(nested) } } },
                                     opener,
                                     vtpty::nativeBeneathReader() };
    CHECK(service.read("worktrees", shortName + "/.git/config") == std::unexpected(FileError::NotGranted));
    // Only the readable root's own directory was opened by path; the nested root never was.
    CHECK(std::ranges::all_of(opener.opened,
                              [&](std::string const& path) { return path == core::pathToUtf8(wt); }));
}

TEST_CASE("FileReadService: a Windows root removed while held is read again once it is back",
          "[vtextension][file-read]")
{
    // Removed under legacy delete semantics, a directory stays delete-pending at its path while any handle
    // holds it, and an open of that path is refused: the held handle is let go before the path is opened
    // again.
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-removed" };
    auto const wt = std::filesystem::canonical(temp.path()) / "wt";
    auto const populate = [&wt] {
        std::filesystem::create_directories(wt / "run-1");
        writeFile(wt / "run-1" / ".git", "gitdir: x\n");
    };
    populate();

    auto opener = RecordingOpener {};
    auto handedOut = std::vector<HANDLE> {};
    auto heldAtOpen = std::ptrdiff_t { 0 };
    opener.beforeOpen = [&] {
        heldAtOpen += std::ranges::count_if(handedOut, [](HANDLE handle) {
            auto flags = DWORD { 0 };
            return GetHandleInformation(handle, &flags) != FALSE;
        });
    };
    opener.onOpened = [&](vtpty::DirectoryHandle const& handle) {
        handedOut.push_back(handle.nativeHandle());
    };
    auto service = FileReadService { readingWorktrees(),
                                     { { .label = "worktrees", .paths = { core::pathToUtf8(wt) } } },
                                     opener,
                                     vtpty::nativeBeneathReader() };
    REQUIRE(service.read("worktrees", "run-1/.git").has_value());

    std::filesystem::remove_all(wt / "run-1");
    REQUIRE(vtpty::testing::removeLegacy(wt).has_value());
    CHECK(service.read("worktrees", "run-1/.git") == std::unexpected(FileError::DirectoryGone));
    CHECK(heldAtOpen == 0);

    populate();
    auto const content = service.read("worktrees", "run-1/.git");
    REQUIRE(content.has_value());
    CHECK(content->bytes == bytesOf("gitdir: x\n"));
    CHECK(opener.opened == std::vector<std::string>(3, core::pathToUtf8(wt)));
}

TEST_CASE("FileReadService: a junction inside a Windows root escapes", "[vtextension][file-read]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-junction" };
    auto const outsideTemp = core::testing::ScopedTempDir { "contour-file-read-outside" };
    auto const wt = std::filesystem::canonical(temp.path());
    auto const outside = std::filesystem::canonical(outsideTemp.path());
    std::filesystem::create_directories(wt / "r1");
    writeFile(outside / "secret", "secret\n");
    REQUIRE(vtpty::testing::createJunction(wt / "r1" / "out", outside).has_value());

    auto opener = RecordingOpener {};
    auto service = FileReadService { readingWorktrees(),
                                     { { .label = "worktrees", .paths = { core::pathToUtf8(wt) } } },
                                     opener,
                                     vtpty::nativeBeneathReader() };
    CHECK(service.read("worktrees", "r1/out/secret") == std::unexpected(FileError::Escapes));
}

TEST_CASE("FileReadService: a Windows stream or device name is a bad argument", "[vtextension][file-read]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-file-read-names" };
    auto const wt = std::filesystem::canonical(temp.path());
    std::filesystem::create_directories(wt / "r1");
    writeFile(wt / "r1" / "STATUS.md", "# Board\n");

    auto opener = RecordingOpener {};
    auto service = FileReadService { readingWorktrees(),
                                     { { .label = "worktrees", .paths = { core::pathToUtf8(wt) } } },
                                     opener,
                                     vtpty::nativeBeneathReader() };
    for (auto const path: std::array { "r1/STATUS.md:x"sv, "r1/CON"sv, "r1/x."sv })
    {
        CAPTURE(path);
        CHECK(service.read("worktrees", path) == std::unexpected(FileError::BadArgument));
    }
    CHECK(opener.opened.empty());
}

#endif
