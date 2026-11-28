// SPDX-License-Identifier: Apache-2.0
//
// The path planner both resolvers share: what a relative path becomes before any directory is
// touched, under each PathPolicy row. One table, run over both policies on every OS, because the
// in-memory model runs either and the native default is only one of them.
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/detail/PathWalk.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using core::fs::FsErrorCode;
using core::fs::PathPolicy;
using core::fs::posixPathPolicy;
using core::fs::windowsPathPolicy;
using core::fs::detail::checkSymlinkTarget;
using core::fs::detail::EntryKind;
using core::fs::detail::FinalComponent;
using core::fs::detail::planRelativePath;
using core::fs::detail::spliceSymlink;
using core::fs::detail::walkBeneath;
using core::fs::test::outcomeOf;

namespace
{

/// What a path plans to: components, or the code it is refused with.
struct Expected
{
    std::vector<std::string> components;
    std::optional<FsErrorCode> error;
};

Expected accepts(std::vector<std::string> components)
{
    return Expected { .components = std::move(components), .error = std::nullopt };
}

Expected refuses(FsErrorCode code)
{
    return Expected { .components = {}, .error = code };
}

struct Row
{
    std::string_view path;
    Expected posix;
    Expected windows;
};

void checkPlan(std::string_view path, PathPolicy const& policy, Expected const& expected)
{
    CAPTURE(path);
    auto const plan = planRelativePath(path, policy);
    if (expected.error)
    {
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error().code == *expected.error);
    }
    else
    {
        REQUIRE(plan.has_value());
        CHECK(plan->components == expected.components);
    }
}

} // namespace

TEST_CASE("planRelativePath: each path under each policy", "[fs][pathwalk]")
{
    using enum FsErrorCode;
    auto const row = GENERATE(Row { "/etc", refuses(NotPermitted), refuses(NotPermitted) },
                              Row { "C:x", accepts({ "C:x" }), refuses(Invalid) },
                              Row { "\\\\srv\\x", accepts({ "\\\\srv\\x" }), refuses(NotPermitted) },
                              Row { "a:b", accepts({ "a:b" }), refuses(Invalid) },
                              Row { "a\\b", accepts({ "a\\b" }), accepts({ "a", "b" }) },
                              Row { std::string_view { "a\0b", 3 }, refuses(Invalid), refuses(Invalid) },
                              Row { "a\xff", refuses(IllegalByteSequence), refuses(IllegalByteSequence) },
                              Row { "\xc0\xaf", refuses(IllegalByteSequence), refuses(IllegalByteSequence) },
                              Row { "a/./b//c", accepts({ "a", "b", "c" }), accepts({ "a", "b", "c" }) },
                              Row { "a/../b", accepts({ "a", "..", "b" }), accepts({ "a", "..", "b" }) },
                              Row { "", refuses(NoEntry), refuses(NoEntry) },
                              Row { ".", accepts({ "." }), accepts({ "." }) },
                              Row { "./.", accepts({ "." }), accepts({ "." }) },
                              Row { "nul.txt", accepts({ "nul.txt" }), refuses(NotPermitted) },
                              Row { "dir/CON", accepts({ "dir", "CON" }), refuses(NotPermitted) },
                              Row { "COM\xc2\xb9", accepts({ "COM\xc2\xb9" }), refuses(NotPermitted) },
                              Row { "CONIN$", accepts({ "CONIN$" }), refuses(NotPermitted) },
                              Row { "console", accepts({ "console" }), accepts({ "console" }) },
                              Row { "x.", accepts({ "x." }), refuses(Invalid) },
                              Row { "x ", accepts({ "x " }), refuses(Invalid) },
                              Row { "a|b", accepts({ "a|b" }), refuses(Invalid) },
                              Row { "caf\xc3\xa9", accepts({ "caf\xc3\xa9" }), accepts({ "caf\xc3\xa9" }) });

    SECTION("posix")
    {
        checkPlan(row.path, posixPathPolicy(), row.posix);
    }
    SECTION("windows")
    {
        checkPlan(row.path, windowsPathPolicy(), row.windows);
    }
}

TEST_CASE("spliceSymlink: a target replaces the link's component", "[fs][pathwalk]")
{
    auto plan = planRelativePath("a/link/c", posixPathPolicy());
    REQUIRE(plan.has_value());
    REQUIRE(spliceSymlink(*plan, 1, "x/../y", posixPathPolicy()).has_value());
    CHECK(plan->components == std::vector<std::string> { "a", "x", "..", "y", "c" });
}

TEST_CASE("spliceSymlink: a target is split on the policy's target separators", "[fs][pathwalk]")
{
    SECTION("posix keeps a backslash in the name")
    {
        auto plan = planRelativePath("link", posixPathPolicy());
        REQUIRE(plan.has_value());
        REQUIRE(spliceSymlink(*plan, 0, "..\\x", posixPathPolicy()).has_value());
        CHECK(plan->components == std::vector<std::string> { "..\\x" });
    }
    SECTION("windows splits on it")
    {
        auto plan = planRelativePath("link", windowsPathPolicy());
        REQUIRE(plan.has_value());
        REQUIRE(spliceSymlink(*plan, 0, "..\\x", windowsPathPolicy()).has_value());
        CHECK(plan->components == std::vector<std::string> { "..", "x" });
    }
}

TEST_CASE("spliceSymlink: absolute and empty targets are refused", "[fs][pathwalk]")
{
    auto const& policy = GENERATE(std::cref(posixPathPolicy()), std::cref(windowsPathPolicy()));
    auto plan = planRelativePath("link", policy);
    REQUIRE(plan.has_value());
    auto const before = plan->components;
    CHECK(outcomeOf(spliceSymlink(*plan, 0, "/etc/passwd", policy)) == FsErrorCode::NotPermitted);
    CHECK(outcomeOf(spliceSymlink(*plan, 0, "", policy)) == FsErrorCode::NoEntry);
    CHECK(plan->components == before);
}

TEST_CASE("spliceSymlink: a target of the directory itself leaves the rest", "[fs][pathwalk]")
{
    auto plan = planRelativePath("a/link/b", posixPathPolicy());
    REQUIRE(plan.has_value());
    REQUIRE(spliceSymlink(*plan, 1, ".", posixPathPolicy()).has_value());
    CHECK(plan->components == std::vector<std::string> { "a", "b" });
}

TEST_CASE("planRelativePath: a trailing separator or dot asks for a directory", "[fs][pathwalk]")
{
    auto const& policy = GENERATE(std::cref(posixPathPolicy()), std::cref(windowsPathPolicy()));
    struct Case
    {
        std::string_view path;
        std::vector<std::string> components;
        FinalComponent finalComponent;
    };
    auto const testCase = GENERATE(Case { "a", { "a" }, FinalComponent::Any },
                                   Case { "a/", { "a" }, FinalComponent::Directory },
                                   Case { "a//", { "a" }, FinalComponent::Directory },
                                   Case { "a/.", { "a", "." }, FinalComponent::Directory },
                                   Case { "a/./", { "a", "." }, FinalComponent::Directory },
                                   Case { "a/./.", { "a", "." }, FinalComponent::Directory },
                                   Case { "a/b/..", { "a", "b", ".." }, FinalComponent::Any },
                                   Case { "a/./b", { "a", "b" }, FinalComponent::Any });
    CAPTURE(testCase.path);
    auto const plan = planRelativePath(testCase.path, policy);
    REQUIRE(plan.has_value());
    CHECK(plan->components == testCase.components);
    CHECK(plan->finalComponent == testCase.finalComponent);
}

TEST_CASE("spliceSymlink: a final link whose target ends in a separator asks for a directory",
          "[fs][pathwalk]")
{
    auto plan = planRelativePath("a/link", posixPathPolicy());
    REQUIRE(plan.has_value());
    REQUIRE(spliceSymlink(*plan, 1, "d/", posixPathPolicy()).has_value());
    CHECK(plan->finalComponent == FinalComponent::Directory);

    auto inner = planRelativePath("link/b", posixPathPolicy());
    REQUIRE(inner.has_value());
    REQUIRE(spliceSymlink(*inner, 0, "d/", posixPathPolicy()).has_value());
    CHECK(inner->finalComponent == FinalComponent::Any);
}

TEST_CASE("checkSymlinkTarget: what a link may not hold", "[fs][pathwalk]")
{
    using enum FsErrorCode;
    CHECK(outcomeOf(checkSymlinkTarget("", posixPathPolicy())) == NoEntry);
    CHECK(outcomeOf(checkSymlinkTarget(std::string_view { "a\0b", 3 }, posixPathPolicy())) == Invalid);
    CHECK(outcomeOf(checkSymlinkTarget("/etc", posixPathPolicy())) == NotPermitted);
    CHECK(checkSymlinkTarget("../x", posixPathPolicy()).has_value());
    CHECK(checkSymlinkTarget("C:x", posixPathPolicy()).has_value());
    CHECK(outcomeOf(checkSymlinkTarget("C:x", windowsPathPolicy())) == NotPermitted);
    CHECK(outcomeOf(checkSymlinkTarget("C:\\x", windowsPathPolicy())) == NotPermitted);
    CHECK(outcomeOf(checkSymlinkTarget("\\\\srv\\x", windowsPathPolicy())) == NotPermitted);
    CHECK(outcomeOf(checkSymlinkTarget("a/b:c", windowsPathPolicy())) == Invalid);
}

namespace
{

/// A cursor over one directory where every name is a link to itself, counting what it is asked:
/// whatever the walk enforces, it enforces without the cursor's help.
class SelfLinkCursor final: public core::fs::detail::IBeneathCursor
{
  public:
    [[nodiscard]] bool atRoot() const noexcept override { return true; }
    [[nodiscard]] core::fs::FsResult<EntryKind> enter(std::string const&) override
    {
        return EntryKind::SymbolicLink;
    }
    void leave() override { ++leaves; }
    [[nodiscard]] core::fs::FsResult<EntryKind> inspect(std::string const&) override
    {
        return EntryKind::SymbolicLink;
    }
    [[nodiscard]] core::fs::FsResult<std::string> readLink(std::string const& name) override
    {
        ++reads;
        return name;
    }
    [[nodiscard]] core::fs::FsResult<core::fs::detail::Shortcut> shortcut(
        std::span<std::string const>) override
    {
        return core::fs::detail::Shortcut::Declined;
    }
    void restart() override {}

    std::size_t reads = 0;
    std::size_t leaves = 0;
};

core::fs::detail::WalkRules followingRules(std::size_t maxSymlinkHops)
{
    return core::fs::detail::WalkRules { .policy = &posixPathPolicy(),
                                         .finalPolicy = core::fs::SymlinkPolicy::Follow,
                                         .onLink = core::fs::detail::TrailingSlashOnLink::Follow,
                                         .maxSymlinkHops = maxSymlinkHops,
                                         .kernelSeparators = "/" };
}

} // namespace

TEST_CASE("walkBeneath: the walk owns the hop limit and `..` at the root", "[fs][walk]")
{
    auto const hops = GENERATE(std::size_t { 0 }, std::size_t { 3 }, core::fs::DefaultMaxSymlinkHops);
    CAPTURE(hops);
    auto cursor = SelfLinkCursor {};
    auto const looped =
        walkBeneath(cursor, planRelativePath("a/b", posixPathPolicy()).value(), followingRules(hops));
    REQUIRE_FALSE(looped.has_value());
    CHECK(looped.error().code == FsErrorCode::Loop);
    CHECK(cursor.reads == hops);

    auto const climbed =
        walkBeneath(cursor, planRelativePath("../x", posixPathPolicy()).value(), followingRules(hops));
    REQUIRE_FALSE(climbed.has_value());
    CHECK(climbed.error().code == FsErrorCode::NotPermitted);
    CHECK(cursor.leaves == 0);
}
