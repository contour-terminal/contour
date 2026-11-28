// SPDX-License-Identifier: Apache-2.0
//
// The openat2 opener through a scripted system call: the construction-time probe, the bounded EAGAIN
// retry that ends in the walk for that call, and the kernel's own refusal of a directory swapped for
// an outward link just before the call.
#include <core/fs/FsError.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/linux/Openat2.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sys/syscall.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>

// Only the ABI cross-check reads the kernel's spelling, and only where the headers have it: the
// module itself builds without them (`Openat2.hpp`), and so must its tests.
#if __has_include(<linux/openat2.h>)
    #include <linux/openat2.h>
#endif

using core::fs::FsErrorCode;
using core::fs::SymlinkPolicy;
using core::fs::detail::IOpenat2System;
using core::fs::detail::makeOpenat2Opener;
using core::fs::detail::Openat2Retries;
using core::fs::detail::systemOpenat2;
using core::fs::detail::UniqueHandle;
using core::fs::detail::WalkRules;

namespace
{

/// The kernel's openat2, answering `EAGAIN` the first @c refusals times after the probe, and
/// running a hook before the first real call.
class ScriptedOpenat2 final: public IOpenat2System
{
  public:
    ScriptedOpenat2(std::size_t refusals, int probeErrno = EBADF):
        _refusals { refusals }, _probeErrno { probeErrno }
    {
    }

    [[nodiscard]] int openat2(int directory,
                              char const* path,
                              core::fs::detail::OpenHow const& how) const noexcept override
    {
        if (directory == -1)
        {
            errno = _probeErrno;
            return -1;
        }
        ++_calls;
        if (_calls <= _refusals)
        {
            errno = EAGAIN;
            return -1;
        }
        if (_beforeCall)
            std::exchange(_beforeCall, nullptr)();
        return _kernel->openat2(directory, path, how);
    }

    /// @return How many calls were made beside the probe.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls; }

    /// @param hook What runs just before the first call that reaches the kernel.
    void beforeCall(std::function<void()> hook) { _beforeCall = std::move(hook); }

  private:
    std::shared_ptr<IOpenat2System const> _kernel = systemOpenat2();
    std::size_t _refusals;
    int _probeErrno;
    // The seam's call is const, as the kernel's is; the script counts what it was asked.
    mutable std::size_t _calls = 0;
    mutable std::function<void()> _beforeCall;
};

/// `<temp>/root/d/inside.txt`, with the root held open.
class Tree
{
  public:
    Tree()
    {
        std::filesystem::create_directories(rootPath() / "d");
        std::filesystem::create_directory(rootPath() / "d" / "e");
        _root = UniqueHandle { ::open(rootPath().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        if (_root.get() < 0)
            throw std::runtime_error { "Tree: the root did not open" };
    }

    [[nodiscard]] int root() const noexcept { return _root.get(); }

    [[nodiscard]] std::filesystem::path rootPath() const { return _directory.path() / "root"; }

  private:
    core::testing::ScopedTempDir _directory { "core-cpp-fs-openat2" };
    UniqueHandle _root { -1 };
};

WalkRules rules()
{
    return WalkRules { .policy = &core::fs::posixPathPolicy(),
                       .finalPolicy = SymlinkPolicy::NoFollow,
                       .onLink = core::fs::detail::TrailingSlashOnLink::Follow,
                       .maxSymlinkHops = core::fs::DefaultMaxSymlinkHops,
                       .kernelSeparators = "/" };
}

core::fs::detail::PathPlan plan(std::string const& path)
{
    return core::fs::detail::planRelativePath(path, core::fs::posixPathPolicy()).value();
}

} // namespace

TEST_CASE("openat2: the probe tells a missing call from one that is there", "[fs][openat2]")
{
    CHECK(makeOpenat2Opener(std::make_shared<ScriptedOpenat2 const>(0, EBADF)) != nullptr);
    CHECK(makeOpenat2Opener(std::make_shared<ScriptedOpenat2 const>(0, ENOSYS)) == nullptr);
    CHECK(makeOpenat2Opener(std::make_shared<ScriptedOpenat2 const>(0, EPERM)) == nullptr);
}

TEST_CASE("openat2: EAGAIN is retried, then that call walks instead", "[fs][openat2]")
{
    if (makeOpenat2Opener(systemOpenat2()) == nullptr)
        SKIP("this kernel, or a seccomp filter, refuses openat2");
    auto const tree = Tree {};
    auto const refusals = GENERATE(
        std::size_t { 0 }, std::size_t { 3 }, Openat2Retries, Openat2Retries + 1, std::size_t { 100 });
    CAPTURE(refusals);
    auto const system = std::make_shared<ScriptedOpenat2>(refusals);
    auto const opener = makeOpenat2Opener(system);
    REQUIRE(opener != nullptr);

    auto const resolved = core::fs::detail::resolveBeneath(tree.root(), plan("d/e/x"), rules(), opener.get());
    REQUIRE(resolved.has_value());
    CHECK(resolved->name == "x");
    CHECK(resolved->held.get() != -1);
    // At most one call and its retries; past them, the walk resolved `d/e` itself.
    CHECK(system->calls() == std::min(refusals, Openat2Retries + 1) + (refusals <= Openat2Retries ? 1 : 0));
}

TEST_CASE("openat2: a directory swapped for an outward link just before the call is refused", "[fs][escape]")
{
    if (makeOpenat2Opener(systemOpenat2()) == nullptr)
        SKIP("this kernel, or a seccomp filter, refuses openat2");
    auto const tree = Tree {};
    auto const system = std::make_shared<ScriptedOpenat2>(0);
    system->beforeCall([&] {
        std::filesystem::rename(tree.rootPath() / "d", tree.rootPath() / "d.directory");
        std::filesystem::create_directory_symlink("../", tree.rootPath() / "d");
    });
    auto const opener = makeOpenat2Opener(system);
    REQUIRE(opener != nullptr);

    auto const resolved = core::fs::detail::resolveBeneath(tree.root(), plan("d/e/x"), rules(), opener.get());
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == FsErrorCode::NotPermitted);
}

TEST_CASE("openat2: the ABI spelled here is the kernel headers'", "[fs][native]")
{
#if __has_include(<linux/openat2.h>)
    // The module builds without <linux/openat2.h>; where the headers are present, they must agree.
    using core::fs::detail::OpenHow;
    using core::fs::detail::Resolve;
    STATIC_CHECK(sizeof(OpenHow) == sizeof(::open_how));
    STATIC_CHECK(offsetof(OpenHow, flags) == offsetof(::open_how, flags));
    STATIC_CHECK(offsetof(OpenHow, mode) == offsetof(::open_how, mode));
    STATIC_CHECK(offsetof(OpenHow, resolve) == offsetof(::open_how, resolve));
    STATIC_CHECK(static_cast<std::uint64_t>(Resolve::Beneath) == RESOLVE_BENEATH);
    STATIC_CHECK(static_cast<std::uint64_t>(Resolve::NoMagicLinks) == RESOLVE_NO_MAGICLINKS);
    #ifdef SYS_openat2
    STATIC_CHECK(core::fs::detail::Openat2Syscall == SYS_openat2);
    #endif
#else
    SKIP("these kernel headers have no <linux/openat2.h> to compare against");
#endif
}
