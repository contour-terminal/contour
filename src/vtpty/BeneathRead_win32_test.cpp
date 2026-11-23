// SPDX-License-Identifier: Apache-2.0
//
// A file read beneath a directory handle on Windows, against a real temporary directory: what it reads, and
// every way a path could leave the directory, reach a fenced one, or keep a handle open.
//
// The POSIX suite's cases not ported here, and why:
// - "a named pipe answers not-regular without blocking": there are no FIFOs beneath an NTFS directory; the
//   closest, an AF_UNIX socket, is a reparse tag, which windowsFileKindOf()'s own suite classifies.
// - "a real mount crossing escapes", and its openat2 probe of /proc: Linux-only; a Windows mount point is a
//   name surrogate, which the junction cases cover.
// - "a symlink inside the root to a private file outside is refused": merged into the symlink case.

#ifdef _WIN32

    #include <vtpty/BeneathRead.hpp>
    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/UniqueHandle.hpp>
    #include <vtpty/Win32BeneathRead.hpp>
    #include <vtpty/testing/Junction.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <expected>
    #include <filesystem>
    #include <fstream>
    #include <functional>
    #include <map>
    #include <memory>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <system_error>
    #include <utility>
    #include <vector>

    #include <Windows.h>

using core::pathToUtf8;
using vtpty::BeneathReadError;
using namespace std::string_view_literals;

namespace
{

constexpr auto Limit = std::size_t { 64 } * 1024;

/// Writes @p bytes to @p path, replacing what was there.
void writeFile(std::filesystem::path const& path, std::string const& bytes)
{
    auto out = std::ofstream { path, std::ios::binary | std::ios::trunc };
    out << bytes;
}

/// @return The milliseconds since the Unix epoch, now.
[[nodiscard]] uint64_t nowUnixMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

/// @return @p path's 8.3 spelling; @p path itself when it has none.
[[nodiscard]] std::filesystem::path shortPathOf(std::filesystem::path const& path)
{
    auto shortName = std::wstring(MAX_PATH, L'\0');
    auto const length =
        GetShortPathNameW(path.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    if (length == 0 || length >= shortName.size())
        return path;
    shortName.resize(length);
    return std::filesystem::path { shortName };
}

/// A temporary directory with a root beneath it, held open as the service holds a root, and an outside
/// beside it that the root must never reach; read through this machine's Win32 calls.
struct Tree
{
    Tree() = default;

    /// @return @p path, opened as the service opens a root.
    [[nodiscard]] static vtpty::DirectoryHandle open(std::filesystem::path const& path)
    {
        auto opened = vtpty::nativeDirectoryOpener().open(pathToUtf8(path));
        REQUIRE(opened.has_value());
        return std::move(*opened);
    }

    /// Reads @p components beneath the root.
    [[nodiscard]] std::expected<vtpty::BeneathFile, BeneathReadError> read(
        std::vector<std::string_view> const& components) const
    {
        return reader->read(held, components, Limit, {});
    }

    /// Reads @p components beneath the root with @p other, fencing @p fenced off.
    [[nodiscard]] std::expected<vtpty::BeneathFile, BeneathReadError> readWith(
        vtpty::IBeneathReader& other,
        std::vector<std::string_view> const& components,
        std::vector<vtpty::FileIdentity> const& fenced = {}) const
    {
        return other.read(held, components, Limit, fenced);
    }

    core::testing::ScopedTempDir temp { "contour-beneathread" };
    std::filesystem::path root = temp.path() / "root";
    std::filesystem::path outside = temp.path() / "outside";
    // Held for the tree's life, opened before any entry is: a handle value a test double names an entry by
    // is then never the root's.
    vtpty::DirectoryHandle held = makeTree();
    vtpty::IWin32BeneathSystem& system = vtpty::nativeWin32BeneathSystem();
    std::unique_ptr<vtpty::IBeneathReader> reader = vtpty::makeWin32BeneathReader(system);

  private:
    [[nodiscard]] vtpty::DirectoryHandle makeTree() const
    {
        std::filesystem::create_directory(root);
        std::filesystem::create_directory(outside);
        writeFile(outside / "private", "secret");
        return open(root);
    }
};

/// @return @p name in UTF-8.
[[nodiscard]] std::string utf8Of(std::wstring_view name)
{
    return pathToUtf8(std::filesystem::path { std::wstring { name } });
}

/// This machine's Win32 calls, through which a test injects an error or rewrites what is told of an entry, by
/// the name it was opened by (the root's is empty; a re-opened file keeps the name it was looked at by).
class ForwardingWin32System final: public vtpty::IWin32BeneathSystem
{
  public:
    explicit ForwardingWin32System(vtpty::IWin32BeneathSystem& next): _next { next } {}

    [[nodiscard]] std::expected<vtpty::UniqueHandle, DWORD> open(HANDLE parent,
                                                                 std::wstring_view name,
                                                                 vtpty::BeneathEntry entry) override
    {
        auto const named = utf8Of(name);
        if (failOpen)
            if (auto const error = failOpen(named))
                return std::unexpected(*error);
        auto opened = _next.open(parent, name, entry);
        if (opened)
        {
            _names.insert_or_assign(opened->get(), named);
            if (afterOpen)
                afterOpen(named);
        }
        return opened;
    }

    [[nodiscard]] std::expected<vtpty::UniqueHandle, DWORD> reopenForRead(HANDLE looked) override
    {
        ++reopenCalls;
        auto reopened = _next.reopenForRead(looked);
        if (reopened)
            _names.insert_or_assign(reopened->get(), nameOf(looked));
        return reopened;
    }

    [[nodiscard]] std::expected<vtpty::FileStatus, DWORD> status(HANDLE handle) override
    {
        auto const name = nameOf(handle);
        return _next.status(handle).transform([&](vtpty::FileStatus status) {
            if (rewriteStatus)
                rewriteStatus(name, status);
            return status;
        });
    }

    [[nodiscard]] std::expected<std::size_t, DWORD> read(HANDLE handle, std::span<char> into) override
    {
        return _next.read(handle, into);
    }

    /// The Win32 error an open of a name fails with; std::nullopt to ask the system. Asked of every name.
    std::function<std::optional<DWORD>(std::string const& name)> failOpen;
    /// Rewrites what is told of a handle, by the name it was opened by.
    std::function<void(std::string const& name, vtpty::FileStatus&)> rewriteStatus;
    /// Runs once a name has been opened.
    std::function<void(std::string const& name)> afterOpen;
    /// How often a file was re-opened for reading.
    int reopenCalls = 0;

  private:
    [[nodiscard]] std::string nameOf(HANDLE handle) const
    {
        auto const named = _names.find(handle);
        return named == _names.end() ? std::string {} : named->second;
    }

    vtpty::IWin32BeneathSystem& _next;
    std::map<HANDLE, std::string> _names;
};

} // namespace

TEST_CASE("BeneathRead: a regular file is read with its mtime", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "STATUS.md", "# board\n");

    auto const file = tree.read({ "wt", "STATUS.md" });
    REQUIRE(file.has_value());
    CHECK(file->bytes == "# board\n");
    constexpr auto OneMinuteMs = uint64_t { 60'000 };
    CHECK(file->modifiedUnixMs + OneMinuteMs > nowUnixMs());
    CHECK(file->modifiedUnixMs < nowUnixMs() + OneMinuteMs);

    // The production instance is the Windows reader.
    CHECK(tree.readWith(vtpty::nativeBeneathReader(), { "wt", "STATUS.md" }) == file);
}

TEST_CASE("BeneathRead: an empty file and one of exactly the limit are read", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    writeFile(tree.root / "empty", "");
    writeFile(tree.root / "full", std::string(Limit, 'x'));
    CHECK(tree.read({ "empty" }).value().bytes.empty());
    CHECK(tree.read({ "full" }).value().bytes.size() == Limit);
}

TEST_CASE("BeneathRead: a junction component escapes", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    REQUIRE(vtpty::testing::createJunction(tree.root / "link", tree.outside).has_value());
    CHECK(tree.read({ "link", "private" }) == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a junction as the file escapes", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    REQUIRE(vtpty::testing::createJunction(tree.root / "link", tree.outside).has_value());
    CHECK(tree.read({ "link" }) == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a symlink component or file escapes", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    auto directoryError = std::error_code {};
    std::filesystem::create_directory_symlink(tree.outside, tree.root / "dlink", directoryError);
    auto fileError = std::error_code {};
    std::filesystem::create_symlink(tree.outside / "private", tree.root / "flink", fileError);
    if (directoryError || fileError)
        SKIP("no symlink privilege");

    CHECK(tree.read({ "dlink", "private" }) == std::unexpected(BeneathReadError::Escapes));
    CHECK(tree.read({ "flink" }) == std::unexpected(BeneathReadError::Escapes));
    // A file symlink where a directory is expected is a link too, whatever it points at.
    CHECK(tree.read({ "flink", "private" }) == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a hard link escapes", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    REQUIRE(CreateHardLinkW((tree.root / "hard").c_str(), (tree.outside / "private").c_str(), nullptr)
            != FALSE);
    CHECK(tree.read({ "hard" }) == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a file on another volume escapes", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "file", "elsewhere");
    writeFile(tree.root / "file", "elsewhere");
    auto system = ForwardingWin32System { tree.system };
    auto const reader = vtpty::makeWin32BeneathReader(system);

    SECTION("the file")
    {
        system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
            if (name == "file")
                ++status.identity.device;
        };
        CHECK(tree.readWith(*reader, { "file" }) == std::unexpected(BeneathReadError::Escapes));
    }
    SECTION("a directory on the way")
    {
        system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
            if (name == "wt")
                ++status.identity.device;
        };
        CHECK(tree.readWith(*reader, { "wt", "file" }) == std::unexpected(BeneathReadError::Escapes));
    }
}

TEST_CASE("BeneathRead: a fenced directory is not entered, however it is spelled", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    for (auto const* const name: { "Main", "Main-long-name", "main-spelled" })
    {
        std::filesystem::create_directories(tree.root / name / ".git");
        writeFile(tree.root / name / ".git" / "config", "[core]\n");
    }
    auto const fence = tree.reader->identityOf(tree.held, std::array { "Main"sv });
    REQUIRE(fence.has_value());

    CHECK(tree.readWith(*tree.reader, { "Main", ".git", "config" }, { *fence })
          == std::unexpected(BeneathReadError::Fenced));
    // Windows resolves another case to the same directory: the identity decides, not the spelling.
    CHECK(tree.readWith(*tree.reader, { "main", ".git", "config" }, { *fence })
          == std::unexpected(BeneathReadError::Fenced));
    CHECK(tree.readWith(*tree.reader, { "main-spelled", ".git", "config" }, { *fence }).has_value());

    // And an 8.3 short name, where the volume makes them.
    auto const longName = std::filesystem::path { L"Main-long-name" };
    auto const shortName = pathToUtf8(shortPathOf(tree.root / longName).filename());
    if (shortName == pathToUtf8(longName))
        WARN("no 8.3 names on this volume");
    else
    {
        auto const longFence = tree.reader->identityOf(tree.held, std::array { "Main-long-name"sv });
        REQUIRE(longFence.has_value());
        CHECK(tree.readWith(*tree.reader, { shortName, ".git", "config" }, { *longFence })
              == std::unexpected(BeneathReadError::Fenced));
    }
}

TEST_CASE("BeneathRead: a directory's identity is reached as a read walks", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directories(tree.root / "a" / "b");
    REQUIRE(vtpty::testing::createJunction(tree.root / "link", tree.root / "a").has_value());
    auto const named = tree.reader->identityOf(Tree::open(tree.root / "a" / "b"), {});
    REQUIRE(named.has_value());

    CHECK(tree.reader->identityOf(tree.held, std::array { "a"sv, "b"sv }) == named);
    CHECK(tree.reader->identityOf(tree.held, std::array { "link"sv, "b"sv })
          == std::unexpected(BeneathReadError::Escapes));
    CHECK(tree.reader->identityOf(tree.held, std::array { "gone"sv })
          == std::unexpected(BeneathReadError::DirectoryMissing));
}

TEST_CASE("BeneathRead: a root removed while it is held is directory-missing", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    auto const held = Tree::open(tree.root / "wt");
    std::filesystem::remove(tree.root / "wt");
    for (auto const& components: { std::vector { "run"sv, ".git"sv }, std::vector { ".git"sv } })
        CHECK(tree.reader->read(held, components, Limit, {})
              == std::unexpected(BeneathReadError::DirectoryMissing));
}

TEST_CASE("BeneathRead: a missing directory is directory-missing, a missing file not-regular",
          "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");

    CHECK(tree.read({ "gone", ".git" }) == std::unexpected(BeneathReadError::DirectoryMissing));
    CHECK(tree.read({ "wt", ".git" }) == std::unexpected(BeneathReadError::NotRegular));
    CHECK(tree.read({ ".git" }) == std::unexpected(BeneathReadError::NotRegular));
}

TEST_CASE("BeneathRead: a file where a directory should be is not-regular", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    writeFile(tree.root / "wt", "a file");
    CHECK(tree.read({ "wt", ".git" }) == std::unexpected(BeneathReadError::NotRegular));
}

TEST_CASE("BeneathRead: a .git that became a directory is not-regular", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directories(tree.root / "wt" / ".git");
    CHECK(tree.read({ "wt", ".git" }) == std::unexpected(BeneathReadError::NotRegular));
}

TEST_CASE("BeneathRead: a file over the limit is too-large", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    writeFile(tree.root / "big", std::string(Limit + 1, 'x'));
    CHECK(tree.read({ "big" }) == std::unexpected(BeneathReadError::TooLarge));
}

TEST_CASE("BeneathRead: a file that grows past its size is too-large", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    writeFile(tree.root / "growing", std::string(std::size_t { 70 } * 1024, 'x'));
    auto system = ForwardingWin32System { tree.system };
    system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
        if (name == "growing")
            status.sizeBytes = 10;
    };
    auto const reader = vtpty::makeWin32BeneathReader(system);
    CHECK(tree.readWith(*reader, { "growing" }) == std::unexpected(BeneathReadError::TooLarge));
}

TEST_CASE("BeneathRead: a component that is not one plain Windows name escapes", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "file", "inside");
    auto asked = std::vector<std::string> {};
    auto system = ForwardingWin32System { tree.system };
    system.failOpen = [&](std::string const& name) -> std::optional<DWORD> {
        asked.push_back(name);
        return std::nullopt;
    };
    auto const reader = vtpty::makeWin32BeneathReader(system);

    for (auto const& components: std::vector<std::vector<std::string_view>> {
             {},
             { "wt", "..", "..", "outside", "private" },
             { "..", "outside", "private" },
             { ".", "wt", "file" },
             { "wt", "", "file" },
             { "wt/file" },
             { "wt\\file" },
             { "C:\\Windows\\win.ini" },
             { "wt", "file:x" },
             { "wt", "file::$DATA" },
             { "CON" },
             { "wt", "nul.txt" },
             { "wt", "file." },
             { "wt", "file " },
             { "wt", "fi\0le"sv },
             { "\xff" },
         })
    {
        CAPTURE(components);
        CHECK(tree.readWith(*reader, components) == std::unexpected(BeneathReadError::Escapes));
    }
    CHECK(asked.empty());
    CHECK(std::filesystem::is_directory(tree.root / "wt"));
    CHECK(tree.read({ "wt", "file" }).value().bytes == "inside");
}

TEST_CASE("BeneathRead: names compare as Windows compares them", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "STATUS.md", "# board\n");
    CHECK(tree.read({ "WT", "status.MD" }).value().bytes == "# board\n");
}

TEST_CASE("BeneathRead: a directory that may not be entered is unreadable", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "closed");
    writeFile(tree.root / "closed" / "file", "inside");
    auto system = ForwardingWin32System { tree.system };
    system.failOpen = [](std::string const& name) -> std::optional<DWORD> {
        return name == "closed" ? std::optional { DWORD { ERROR_ACCESS_DENIED } } : std::nullopt;
    };
    auto const reader = vtpty::makeWin32BeneathReader(system);
    CHECK(tree.readWith(*reader, { "closed", "file" }) == std::unexpected(BeneathReadError::Unreadable));
}

TEST_CASE("BeneathRead: a file another process holds exclusively is unreadable", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    writeFile(tree.root / "locked", "mine");
    auto const lock = vtpty::UniqueHandle { CreateFileW((tree.root / "locked").c_str(),
                                                        GENERIC_READ,
                                                        0,
                                                        nullptr,
                                                        OPEN_EXISTING,
                                                        FILE_ATTRIBUTE_NORMAL,
                                                        nullptr) };
    REQUIRE(lock.valid());
    CHECK(tree.read({ "locked" }) == std::unexpected(BeneathReadError::Unreadable));
}

TEST_CASE("BeneathRead: a file open for writing elsewhere is read", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    auto out = std::ofstream { tree.root / "STATUS.md", std::ios::binary };
    out << "# board\n";
    out.flush();
    REQUIRE(out.good());
    CHECK(tree.read({ "STATUS.md" }).value().bytes == "# board\n");
}

TEST_CASE("BeneathRead: a read holds nothing open afterwards", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "STATUS.md", "# board\n");
    REQUIRE(tree.read({ "wt", "STATUS.md" }).has_value());

    // NTFS refuses to rename a directory while anything beneath it is open.
    auto renamed = std::error_code {};
    std::filesystem::rename(tree.root / "wt", tree.root / "wt2", renamed);
    CHECK_FALSE(renamed);
    auto removed = std::error_code {};
    CHECK(std::filesystem::remove(tree.root / "wt2" / "STATUS.md", removed));
    CHECK_FALSE(removed);
}

TEST_CASE("BeneathRead: what is not a file on disk is not-regular", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    writeFile(tree.root / "placeholder", "in the cloud");
    auto system = ForwardingWin32System { tree.system };
    // As windowsFileKindOf() answers a file whose data is recalled on access.
    system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
        if (name == "placeholder")
            status.kind = vtpty::FileKind::Other;
    };
    auto const reader = vtpty::makeWin32BeneathReader(system);
    CHECK(tree.readWith(*reader, { "placeholder" }) == std::unexpected(BeneathReadError::NotRegular));
    CHECK(system.reopenCalls == 0);
}

TEST_CASE("BeneathRead: an unknown file identity is unreadable, never a match", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "STATUS.md", "# board\n");
    auto const device = tree.reader->identityOf(tree.held, {}).value().device;
    auto system = ForwardingWin32System { tree.system };
    auto const reader = vtpty::makeWin32BeneathReader(system);

    SECTION("a directory on the way")
    {
        system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
            if (name == "wt")
                status.identity.inode = status.identity.inodeHigh = 0;
        };
        CHECK(tree.readWith(*reader, { "wt", "STATUS.md" }) == std::unexpected(BeneathReadError::Unreadable));
        CHECK(tree.readWith(*reader, { "wt", "STATUS.md" }, { vtpty::FileIdentity { .device = device } })
              == std::unexpected(BeneathReadError::Unreadable));
    }
    SECTION("the root")
    {
        system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
            if (name.empty())
                status.identity.inode = status.identity.inodeHigh = 0;
        };
        CHECK(tree.readWith(*reader, { "wt", "STATUS.md" }) == std::unexpected(BeneathReadError::Unreadable));
        CHECK(reader->identityOf(tree.held, {}) == std::unexpected(BeneathReadError::Unreadable));
    }
}

TEST_CASE("BeneathRead: an unbacked root is unreadable", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    auto const root = vtpty::DirectoryHandle::unbacked(pathToUtf8(tree.root));
    auto const components = std::array { "f"sv };
    CHECK(tree.reader->read(root, components, Limit, {}) == std::unexpected(BeneathReadError::Unreadable));
    CHECK(tree.reader->identityOf(root, components) == std::unexpected(BeneathReadError::Unreadable));
}

TEST_CASE("BeneathRead: a directory renamed during the walk is still the one walked", "[vtpty][beneath]")
{
    auto const tree = Tree {};
    std::filesystem::create_directories(tree.root / "a" / "b");
    writeFile(tree.root / "a" / "b" / "f", "inside");
    auto system = ForwardingWin32System { tree.system };
    auto swapped = false;
    // NTFS refuses to rename only a directory with something open beneath it; "b" itself may be.
    system.afterOpen = [&](std::string const& name) {
        if (name != "b" || swapped)
            return;
        swapped = true;
        std::filesystem::rename(tree.root / "a" / "b", tree.root / "a" / "b2");
        std::filesystem::create_directory(tree.root / "a" / "b");
        writeFile(tree.root / "a" / "b" / "f", "planted");
    };
    auto const reader = vtpty::makeWin32BeneathReader(system);

    auto const read = tree.readWith(*reader, { "a", "b", "f" });
    REQUIRE(swapped);
    CHECK(read.value().bytes == "inside");
}

#endif
