// SPDX-License-Identifier: Apache-2.0
//
// The name rules as queries: which symbolic-link targets name a root. One table per policy, both run
// on every OS, since the in-memory model runs either and a WASI host answers `readlink-at` from it.
#include <core/fs/FsError.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/detail/PathWalk.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string_view>

using core::fs::isRootedLinkTarget;
using core::fs::posixPathPolicy;
using core::fs::windowsPathPolicy;

namespace
{

/// A target and whether it names a root.
struct Row
{
    std::string_view target;
    bool rooted = false;
};

} // namespace

TEST_CASE("isRootedLinkTarget follows the table", "[fs][pathpolicy]")
{
    SECTION("posix")
    {
        auto const row = GENERATE(Row { .target = "/etc", .rooted = true },
                                  Row { .target = "/", .rooted = true },
                                  Row { .target = "\\x", .rooted = false },
                                  Row { .target = "C:x", .rooted = false },
                                  Row { .target = "C:", .rooted = false },
                                  Row { .target = "a/b", .rooted = false },
                                  Row { .target = "../x", .rooted = false },
                                  Row { .target = "", .rooted = false });
        CAPTURE(row.target);
        CHECK(isRootedLinkTarget(row.target, posixPathPolicy()) == row.rooted);
    }
    SECTION("windows")
    {
        auto const row = GENERATE(Row { .target = "\\x", .rooted = true },
                                  Row { .target = "/x", .rooted = true },
                                  Row { .target = "C:", .rooted = true },
                                  Row { .target = "C:x", .rooted = true },
                                  Row { .target = "C:\\x", .rooted = true },
                                  Row { .target = "\\\\server\\share", .rooted = true },
                                  Row { .target = "\\\\?\\C:\\", .rooted = true },
                                  Row { .target = "a\\b", .rooted = false },
                                  Row { .target = "a\\C:", .rooted = false },
                                  Row { .target = "..\\x", .rooted = false },
                                  Row { .target = "", .rooted = false });
        CAPTURE(row.target);
        CHECK(isRootedLinkTarget(row.target, windowsPathPolicy()) == row.rooted);
    }
}

TEST_CASE("isRootedLinkTarget is what a followed link is refused by", "[fs][pathpolicy]")
{
    // The walk's splice asks the same rule: a rooted target is NotPermitted, a relative one splices.
    auto const policy = GENERATE(&posixPathPolicy, &windowsPathPolicy);
    for (auto const target:
         { std::string_view { "/etc" }, std::string_view { "C:x" }, std::string_view { "a" } })
    {
        CAPTURE(target);
        auto plan = core::fs::detail::planRelativePath("link", policy());
        REQUIRE(plan.has_value());
        auto const spliced = core::fs::detail::spliceSymlink(*plan, 0, target, policy());
        if (isRootedLinkTarget(target, policy()))
        {
            REQUIRE_FALSE(spliced.has_value());
            CHECK(spliced.error().code == core::fs::FsErrorCode::NotPermitted);
        }
        else
            CHECK(spliced.has_value());
    }
}
