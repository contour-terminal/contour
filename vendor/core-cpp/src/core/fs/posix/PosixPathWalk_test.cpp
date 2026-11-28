// SPDX-License-Identifier: Apache-2.0
//
// The component walker against a directory swapped for an outward link at a chosen step, not by
// timing: a cursor wrapped around the real one makes the swap just before or just after it enters
// the directory, or just before it reads the link back. Whichever step it lands on, the walk either
// keeps the directory it holds or meets the link and refuses it.
#include <core/fs/FsError.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/posix/PosixPathWalk.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

using core::fs::FsErrorCode;
using core::fs::FsResult;
using core::fs::SymlinkPolicy;
using core::fs::detail::EntryKind;
using core::fs::detail::IBeneathCursor;
using core::fs::detail::PosixCursor;
using core::fs::detail::Shortcut;
using core::fs::detail::UniqueHandle;
using core::fs::detail::WalkRules;

namespace
{

/// `<temp>/root/d/inside.txt` holding "inside", and the decoy `<temp>/inside.txt` holding "outside",
/// where `d -> ../` would lead; the root held open.
class SwapTree
{
  public:
    SwapTree()
    {
        std::filesystem::create_directories(rootPath() / "d");
        write(rootPath() / "d" / "inside.txt", "inside");
        write(_directory.path() / "inside.txt", "outside");
        _root = UniqueHandle { ::open(rootPath().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        if (_root.get() < 0)
            throw std::runtime_error { "SwapTree: the root did not open" };
    }

    [[nodiscard]] int root() const noexcept { return _root.get(); }

    /// Puts the link `d -> ../` where the directory was.
    void swapInLink() const
    {
        std::filesystem::rename(rootPath() / "d", rootPath() / "d.directory");
        std::filesystem::create_directory_symlink("../", rootPath() / "d");
    }

    /// Puts the directory back where the link is.
    void swapInDirectory() const
    {
        std::filesystem::remove(rootPath() / "d");
        std::filesystem::rename(rootPath() / "d.directory", rootPath() / "d");
    }

    /// @return What @p name holds beneath @p directory, read without following.
    [[nodiscard]] static std::string readAt(int directory, std::string const& name)
    {
        auto const file =
            UniqueHandle { ::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC) };
        auto text = std::string(64, '\0');
        auto const length = ::read(file.get(), text.data(), text.size());
        text.resize(length < 0 ? 0 : static_cast<std::size_t>(length));
        return text;
    }

  private:
    [[nodiscard]] std::filesystem::path rootPath() const { return _directory.path() / "root"; }

    static void write(std::filesystem::path const& path, std::string const& text)
    {
        auto stream = std::ofstream { path };
        stream << text;
    }

    core::testing::ScopedTempDir _directory { "core-cpp-fs-walk" };
    UniqueHandle _root { -1 };
};

/// When a hook runs, relative to the cursor's steps.
struct Hooks
{
    std::function<void()> beforeEnter;
    std::function<void()> afterEnter;
    std::function<void()> beforeReadLink;
};

/// The real cursor, with a hook at each step.
class HookedCursor final: public IBeneathCursor
{
  public:
    HookedCursor(PosixCursor& inner, Hooks hooks): _inner { &inner }, _hooks { std::move(hooks) } {}

    [[nodiscard]] bool atRoot() const noexcept override { return _inner->atRoot(); }

    [[nodiscard]] FsResult<EntryKind> enter(std::string const& name) override
    {
        run(_hooks.beforeEnter);
        auto entered = _inner->enter(name);
        run(_hooks.afterEnter);
        return entered;
    }

    void leave() override { _inner->leave(); }

    [[nodiscard]] FsResult<EntryKind> inspect(std::string const& name) override
    {
        return _inner->inspect(name);
    }

    [[nodiscard]] FsResult<std::string> readLink(std::string const& name) override
    {
        run(_hooks.beforeReadLink);
        return _inner->readLink(name);
    }

    [[nodiscard]] FsResult<Shortcut> shortcut(std::span<std::string const> components) override
    {
        return _inner->shortcut(components);
    }

    void restart() override { _inner->restart(); }

  private:
    /// Runs @p hook once, then forgets it.
    static void run(std::function<void()>& hook)
    {
        if (hook)
            std::exchange(hook, nullptr)();
    }

    PosixCursor* _inner;
    Hooks _hooks;
};

WalkRules rules()
{
    return WalkRules { .policy = &core::fs::posixPathPolicy(),
                       .finalPolicy = SymlinkPolicy::Follow,
                       .onLink = core::fs::detail::TrailingSlashOnLink::Follow,
                       .maxSymlinkHops = core::fs::DefaultMaxSymlinkHops,
                       .kernelSeparators = "/" };
}

core::fs::detail::PathPlan plan(std::string const& path)
{
    return core::fs::detail::planRelativePath(path, core::fs::posixPathPolicy()).value();
}

} // namespace

TEST_CASE("walker: a directory swapped for a link before it is entered is met as the link", "[fs][escape]")
{
    auto const tree = SwapTree {};
    auto inner = PosixCursor { tree.root(), nullptr };
    auto cursor = HookedCursor {
        inner, Hooks { .beforeEnter = [&] { tree.swapInLink(); }, .afterEnter = {}, .beforeReadLink = {} }
    };

    auto const walked = core::fs::detail::walkBeneath(cursor, plan("d/inside.txt"), rules());
    REQUIRE_FALSE(walked.has_value());
    CHECK(walked.error().code == FsErrorCode::NotPermitted);
}

TEST_CASE("walker: a directory swapped after it was entered stays the one the walk holds", "[fs][escape]")
{
    auto const tree = SwapTree {};
    auto inner = PosixCursor { tree.root(), nullptr };
    auto cursor = HookedCursor {
        inner, Hooks { .beforeEnter = {}, .afterEnter = [&] { tree.swapInLink(); }, .beforeReadLink = {} }
    };

    auto const walked = core::fs::detail::walkBeneath(cursor, plan("d/inside.txt"), rules());
    REQUIRE(walked.has_value());
    CHECK(walked->name == "inside.txt");
    CHECK(walked->kind == EntryKind::Other);
    CHECK(SwapTree::readAt(inner.current(), walked->name) == "inside");
}

TEST_CASE("walker: a link that is a directory again when it is read is entered after all", "[fs][escape]")
{
    auto const tree = SwapTree {};
    auto inner = PosixCursor { tree.root(), nullptr };
    auto cursor = HookedCursor { inner,
                                 Hooks { .beforeEnter = [&] { tree.swapInLink(); },
                                         .afterEnter = {},
                                         .beforeReadLink = [&] { tree.swapInDirectory(); } } };

    auto const walked = core::fs::detail::walkBeneath(cursor, plan("d/inside.txt"), rules());
    REQUIRE(walked.has_value());
    CHECK(SwapTree::readAt(inner.current(), walked->name) == "inside");
}
