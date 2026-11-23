// SPDX-License-Identifier: Apache-2.0
//
// A file read beneath a directory handle, against a real temporary directory: what it reads, and every way a
// path could leave the directory or block the reader.
#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>

#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef _WIN32
    #include <sys/stat.h>

    #include <array>
    #include <atomic>
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
    #include <thread>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
    #include <unistd.h>

using namespace std::chrono_literals;
using vtpty::BeneathReadError;
using enum vtpty::BeneathResolution;
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

/// A temporary directory with a root beneath it, and an outside beside it that the root must never reach;
/// read through the system calls of this machine, resolved as the test says.
struct Tree
{
    explicit Tree(vtpty::BeneathResolution resolution = vtpty::BeneathResolution::Kernel):
        system { vtpty::makeBeneathSystem(resolution) }, reader { vtpty::makeBeneathReader(*system) }
    {
        std::filesystem::create_directory(root);
        std::filesystem::create_directory(outside);
        writeFile(outside / "private", "secret");
    }

    /// @return @p path, opened as the service opens a root.
    [[nodiscard]] static vtpty::DirectoryHandle open(std::filesystem::path const& path)
    {
        auto opened = vtpty::nativeDirectoryOpener().open(path.string());
        REQUIRE(opened.has_value());
        return std::move(*opened);
    }

    /// Reads @p components beneath the root.
    [[nodiscard]] std::expected<vtpty::BeneathFile, BeneathReadError> read(
        std::vector<std::string_view> const& components) const
    {
        return reader->read(open(root), components, Limit, {});
    }

    /// Reads @p components beneath the root with @p other, fencing @p fenced off.
    [[nodiscard]] std::expected<vtpty::BeneathFile, BeneathReadError> readWith(
        vtpty::IBeneathReader& other,
        std::vector<std::string_view> const& components,
        std::vector<vtpty::FileIdentity> const& fenced = {}) const
    {
        return other.read(open(root), components, Limit, fenced);
    }

    core::testing::ScopedTempDir temp { "contour-beneathread" };
    std::filesystem::path root = temp.path() / "root";
    std::filesystem::path outside = temp.path() / "outside";
    std::unique_ptr<vtpty::IBeneathSystem> system;
    std::unique_ptr<vtpty::IBeneathReader> reader;
};

/// The system calls of this machine, through which a test injects an errno or rewrites what is told of an
/// entry, by the name it was opened or looked at by (the root's is empty).
class ForwardingSystem final: public vtpty::IBeneathSystem
{
  public:
    explicit ForwardingSystem(vtpty::IBeneathSystem& next): _next { next } {}

    [[nodiscard]] std::expected<vtpty::OwnedFd, int> open(int directory,
                                                          std::string const& name,
                                                          vtpty::BeneathTarget target) override
    {
        if (failOpen)
            if (auto const error = failOpen(name))
                return std::unexpected(*error);
        auto opened = _next.open(directory, name, target);
        if (opened)
            _names.insert_or_assign(opened->get(), name);
        return opened;
    }

    [[nodiscard]] std::expected<vtpty::FileStatus, int> statusAt(int directory,
                                                                 std::string const& name) override
    {
        if (failOpen)
            if (auto const error = failOpen(name))
                return std::unexpected(*error);
        return _next.statusAt(directory, name).transform([&](vtpty::FileStatus status) {
            if (rewriteLook)
                rewriteLook(name, status);
            return status;
        });
    }

    [[nodiscard]] std::expected<vtpty::FileStatus, int> status(int descriptor) override
    {
        auto const named = _names.find(descriptor);
        auto const name = named == _names.end() ? std::string {} : named->second;
        return _next.status(descriptor).transform([&](vtpty::FileStatus status) {
            if (rewriteStatus)
                rewriteStatus(name, status);
            return status;
        });
    }

    /// The errno an open or a look of a name fails with; std::nullopt to ask the system.
    std::function<std::optional<int>(std::string const& name)> failOpen;
    /// Rewrites what a look tells of a name.
    std::function<void(std::string const& name, vtpty::FileStatus&)> rewriteLook;
    /// Rewrites what fstat tells of a descriptor, by the name it was opened by.
    std::function<void(std::string const& name, vtpty::FileStatus&)> rewriteStatus;

  private:
    vtpty::IBeneathSystem& _next;
    std::map<int, std::string> _names;
};

/// Reads @p components beneath @p tree's root through @p calls; the reader runs on a second thread under a
/// one-second watchdog that frees a reader blocked on @p fifo by opening it for writing.
/// @return What the read answered, and whether it blocked.
[[nodiscard]] std::pair<std::expected<vtpty::BeneathFile, BeneathReadError>, bool> readUnblocked(
    Tree const& tree, vtpty::IBeneathReader& reader, std::filesystem::path const& fifo)
{
    auto done = std::atomic<bool> { false };
    auto blocked = std::atomic<bool> { false };
    auto watchdog = std::thread { [&] {
        auto const deadline = std::chrono::steady_clock::now() + 1s;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        if (done.load())
            return;
        blocked = true;
        if (auto const writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC); writer >= 0)
            ::close(writer);
    } };
    auto read = tree.readWith(reader, { fifo.filename().native() });
    done = true;
    watchdog.join();
    return { std::move(read), blocked.load() };
}

} // namespace

TEST_CASE("BeneathRead: a regular file is read with its mtime", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / ".git", "gitdir: /repo/.git/worktrees/wt\n");
    auto const times = std::array { timespec { .tv_sec = 1'700'000'000, .tv_nsec = 123'456'789 },
                                    timespec { .tv_sec = 1'700'000'000, .tv_nsec = 123'456'789 } };
    REQUIRE(::utimensat(AT_FDCWD, (tree.root / "wt" / ".git").c_str(), times.data(), 0) == 0);

    auto const file = tree.read({ "wt", ".git" });
    REQUIRE(file.has_value());
    CHECK(file->bytes == "gitdir: /repo/.git/worktrees/wt\n");
    CHECK(file->modifiedUnixMs == 1'700'000'000'123U);
}

TEST_CASE("BeneathRead: an empty file and one of exactly the limit are read", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    writeFile(tree.root / "empty", "");
    writeFile(tree.root / "full", std::string(Limit, 'x'));
    CHECK(tree.read({ "empty" }).value().bytes.empty());
    CHECK(tree.read({ "full" }).value().bytes.size() == Limit);
}

TEST_CASE("BeneathRead: a named pipe answers not-regular without blocking", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    auto const fifo = tree.root / "pipe";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    SECTION("refused by the look")
    {
        auto const [read, blocked] = readUnblocked(tree, *tree.reader, fifo);
        CHECK_FALSE(blocked);
        CHECK(read == std::unexpected(BeneathReadError::NotRegular));
    }
    SECTION("a look that calls it regular: the open does not block, its fstat refuses it")
    {
        auto system = ForwardingSystem { *tree.system };
        system.rewriteLook = [](std::string const& /*name*/, vtpty::FileStatus& status) {
            status.kind = vtpty::FileKind::Regular;
        };
        auto const reader = vtpty::makeBeneathReader(system);
        auto const [read, blocked] = readUnblocked(tree, *reader, fifo);
        CHECK_FALSE(blocked);
        CHECK(read == std::unexpected(BeneathReadError::NotRegular));
    }
}

TEST_CASE("BeneathRead: a symlink component escapes", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "real");
    writeFile(tree.root / "real" / "file", "inside");
    std::filesystem::create_directory_symlink(tree.root / "real", tree.root / "link");
    std::filesystem::create_directory_symlink(tree.outside, tree.root / "out");

    // Even a link that stays inside the root is refused: no link is followed.
    CHECK(tree.read({ "link", "file" }) == std::unexpected(BeneathReadError::Escapes));
    CHECK(tree.read({ "out", "private" }) == std::unexpected(BeneathReadError::Escapes));
    CHECK(tree.read({ "real", "file" }).value().bytes == "inside");
}

TEST_CASE("BeneathRead: a symlink as the file escapes", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    writeFile(tree.root / "target", "inside");
    std::filesystem::create_symlink(tree.root / "target", tree.root / "link");
    std::filesystem::create_symlink(tree.root / "nowhere", tree.root / "dangling");

    CHECK(tree.read({ "link" }) == std::unexpected(BeneathReadError::Escapes));
    CHECK(tree.read({ "dangling" }) == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a symlink inside the root to a private file outside is refused", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "wt");
    std::filesystem::create_symlink(tree.outside / "private", tree.root / "wt" / ".git");

    auto const read = tree.read({ "wt", ".git" });
    CHECK(read == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a hard link escapes", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_hard_link(tree.outside / "private", tree.root / "second-name");
    REQUIRE(std::filesystem::hard_link_count(tree.root / "second-name") == 2);

    CHECK(tree.read({ "second-name" }) == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a file on another device escapes", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "mounted");
    writeFile(tree.root / "mounted" / "file", "elsewhere");
    writeFile(tree.root / "file", "elsewhere");
    auto system = ForwardingSystem { *tree.system };
    auto const reader = vtpty::makeBeneathReader(system);

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
            if (name == "mounted")
                ++status.identity.device;
        };
        CHECK(tree.readWith(*reader, { "mounted", "file" }) == std::unexpected(BeneathReadError::Escapes));
    }
    SECTION("a mount the kernel refuses to cross")
    {
        system.failOpen = [](std::string const& name) -> std::optional<int> {
            return name == "mounted" ? std::optional { EXDEV } : std::nullopt;
        };
        CHECK(tree.readWith(*reader, { "mounted", "file" }) == std::unexpected(BeneathReadError::Escapes));
        CHECK(tree.readWith(*reader, { "mounted" }) == std::unexpected(BeneathReadError::Escapes));
    }
}

TEST_CASE("BeneathRead: a real mount crossing escapes", "[vtpty][beneath]")
{
    auto const slash = Tree::open("/");
    if (!std::filesystem::is_directory("/proc/self"))
        SKIP("no /proc");
    for (auto const resolution: { Kernel, PerComponent })
    {
        CAPTURE(resolution);
        auto const system = vtpty::makeBeneathSystem(resolution);
        auto const reader = vtpty::makeBeneathReader(*system);
        auto const components = std::array { "proc"sv, "self"sv, "status"sv };
        CHECK(reader->read(slash, components, Limit, {}) == std::unexpected(BeneathReadError::Escapes));
    }
    // Resolved by the kernel, /proc is refused at its mount point: procfs is never asked.
    auto const system = vtpty::makeBeneathSystem(Kernel);
    auto const opened = system->open(slash.descriptor(), "proc", vtpty::BeneathTarget::Directory);
    if (opened)
        WARN("this kernel has no openat2: the walk compares devices after each open");
    else
        CHECK(opened.error() == EXDEV);
}

TEST_CASE("BeneathRead: a fenced directory is not entered, however it is spelled", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directories(tree.root / "Main" / ".git");
    writeFile(tree.root / "Main" / ".git" / "config", "[core]\n");
    std::filesystem::create_directories(tree.root / "main-spelled" / ".git");
    writeFile(tree.root / "main-spelled" / ".git" / "config", "[core]\n");
    auto const fence = tree.reader->identityOf(Tree::open(tree.root), std::array { "Main"sv });
    REQUIRE(fence.has_value());

    CHECK(tree.readWith(*tree.reader, { "Main", ".git", "config" }, { *fence })
          == std::unexpected(BeneathReadError::Fenced));
    CHECK(tree.readWith(*tree.reader, { "main-spelled", ".git", "config" }, { *fence }).has_value());

    // A case-insensitive filesystem reaches the same directory by another spelling: the identity decides.
    auto system = ForwardingSystem { *tree.system };
    system.rewriteStatus = [&](std::string const& name, vtpty::FileStatus& status) {
        if (name == "main-spelled")
            status.identity = *fence;
    };
    auto const reader = vtpty::makeBeneathReader(system);
    CHECK(tree.readWith(*reader, { "main-spelled", ".git", "config" }, { *fence })
          == std::unexpected(BeneathReadError::Fenced));
}

TEST_CASE("BeneathRead: a directory's identity is reached as a read walks", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directories(tree.root / "a" / "b");
    std::filesystem::create_directory_symlink(tree.root / "a", tree.root / "link");
    auto const root = Tree::open(tree.root);
    auto const named = tree.reader->identityOf(Tree::open(tree.root / "a" / "b"), {});
    REQUIRE(named.has_value());

    CHECK(tree.reader->identityOf(root, std::array { "a"sv, "b"sv }) == named);
    CHECK(tree.reader->identityOf(root, std::array { "link"sv, "b"sv })
          == std::unexpected(BeneathReadError::Escapes));
    CHECK(tree.reader->identityOf(root, std::array { "gone"sv })
          == std::unexpected(BeneathReadError::DirectoryMissing));

    // Behind a mount the kernel refuses to cross, nothing is asked of the mount.
    auto system = ForwardingSystem { *tree.system };
    system.failOpen = [](std::string const& name) -> std::optional<int> {
        return name == "a" ? std::optional { EXDEV } : std::nullopt;
    };
    auto const reader = vtpty::makeBeneathReader(system);
    CHECK(reader->identityOf(root, std::array { "a"sv, "b"sv })
          == std::unexpected(BeneathReadError::Escapes));
}

TEST_CASE("BeneathRead: a root removed while it is held is directory-missing", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
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
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "wt");

    CHECK(tree.read({ "gone", ".git" }) == std::unexpected(BeneathReadError::DirectoryMissing));
    CHECK(tree.read({ "wt", ".git" }) == std::unexpected(BeneathReadError::NotRegular));
    CHECK(tree.read({ ".git" }) == std::unexpected(BeneathReadError::NotRegular));
}

TEST_CASE("BeneathRead: a file where a directory should be is not-regular", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    writeFile(tree.root / "wt", "a file");
    CHECK(tree.read({ "wt", ".git" }) == std::unexpected(BeneathReadError::NotRegular));
}

TEST_CASE("BeneathRead: a .git that became a directory is not-regular", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directories(tree.root / "wt" / ".git");
    CHECK(tree.read({ "wt", ".git" }) == std::unexpected(BeneathReadError::NotRegular));
}

TEST_CASE("BeneathRead: a file over the limit is too-large", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    writeFile(tree.root / "big", std::string(Limit + 1, 'x'));
    CHECK(tree.read({ "big" }) == std::unexpected(BeneathReadError::TooLarge));
}

TEST_CASE("BeneathRead: a file that grows past fstat's size is too-large", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    writeFile(tree.root / "growing", std::string(std::size_t { 70 } * 1024, 'x'));
    auto system = ForwardingSystem { *tree.system };
    system.rewriteStatus = [](std::string const& name, vtpty::FileStatus& status) {
        if (name == "growing")
            status.sizeBytes = 10;
    };
    auto const reader = vtpty::makeBeneathReader(system);
    CHECK(tree.readWith(*reader, { "growing" }) == std::unexpected(BeneathReadError::TooLarge));
}

TEST_CASE("BeneathRead: a component that is not one plain name escapes", "[vtpty][beneath]")
{
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "wt");
    writeFile(tree.root / "wt" / "file", "inside");
    for (auto const& components: std::vector<std::vector<std::string_view>> {
             {},
             { "wt", "..", "..", "outside", "private" },
             { "..", "outside", "private" },
             { ".", "wt", "file" },
             { "wt", "", "file" },
             { "wt/file" },
             { "/etc/passwd" },
             { "wt", "fi\0le"sv },
         })
    {
        CAPTURE(components);
        CHECK(tree.read(components) == std::unexpected(BeneathReadError::Escapes));
    }
}

TEST_CASE("BeneathRead: a directory that may not be entered is unreadable", "[vtpty][beneath]")
{
    if (::geteuid() == 0)
        SKIP("root enters every directory");
    auto const tree = Tree { GENERATE(Kernel, PerComponent) };
    std::filesystem::create_directory(tree.root / "closed");
    writeFile(tree.root / "closed" / "file", "inside");
    std::filesystem::permissions(tree.root / "closed", std::filesystem::perms::none);
    auto const read = tree.read({ "closed", "file" });
    std::filesystem::permissions(tree.root / "closed", std::filesystem::perms::owner_all);
    CHECK(read == std::unexpected(BeneathReadError::Unreadable));
}

#endif
