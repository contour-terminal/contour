// SPDX-License-Identifier: Apache-2.0
//
// The media policy a composition root grants local sessions until configuration supplies one.

#include <vtbackend/graphics/KittyMediaDefaults.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace vtbackend::kitty_graphics;

namespace
{
[[nodiscard]] bool holds(std::vector<std::filesystem::path> const& roots, std::filesystem::path const& root)
{
    return std::ranges::find(roots, root) != roots.end();
}
} // namespace

TEST_CASE("KittyMediaDefaults.platformTemporaryRoots", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/x" } } };
    auto const roots = platformTemporaryRoots(env);
#ifdef _WIN32
    CHECK(roots.empty());
#else
    CHECK(holds(roots, "/x"));
    #ifdef __APPLE__
    CHECK(holds(roots, "/private/tmp"));
    #else
    CHECK(holds(roots, "/tmp"));
    CHECK(holds(roots, "/dev/shm"));
    #endif
#endif
}

TEST_CASE("KittyMediaDefaults.platformTemporaryRoots_ignores_a_relative_TMPDIR", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "relative" } } };
    CHECK_FALSE(holds(platformTemporaryRoots(env), "relative"));
}

TEST_CASE("KittyMediaDefaults.defaultMediaPolicy", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/x" } } };
    auto const policy = defaultMediaPolicy(env, "/home/nobody-here", 1234);
#ifdef _WIN32
    CHECK(policy.transmission == FileTransmission::Disabled);
#else
    CHECK(policy.transmission == FileTransmission::Enabled);
    CHECK(policy.allowedRoots
          == std::vector<std::filesystem::path> { "/home/nobody-here/.claude/plugins/data" });
    CHECK(policy.temporaryRoots == platformTemporaryRoots(env));
    CHECK(policy.userId == 1234);
    CHECK(policy.sizeLimit == MaxChunkedPayloadSize);
#endif
}

TEST_CASE("KittyMediaDefaults.defaultMediaPolicy_without_a_home", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment {};
    CHECK(defaultMediaPolicy(env, "", 1234).allowedRoots.empty());
}
