// SPDX-License-Identifier: Apache-2.0
#include <crispy/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <vthost/SocketPath.hpp>

using vthost::muxSocketPath;

namespace
{
/// Stands in for whatever identifies the user on the host running the suite, so the fallback
/// branch's expectations are exact rather than "contains something".
constexpr auto TestUser = "1000";
} // namespace

TEST_CASE("an explicit --socket path wins over everything", "[vthost][socketpath]")
{
    CHECK(muxSocketPath("default", "/tmp/explicit.sock", "/env/mux", "/run/user/1000", TestUser)
          == "/tmp/explicit.sock");
}

TEST_CASE("$CONTOUR_MUX overrides the derived path", "[vthost][socketpath]")
{
    CHECK(muxSocketPath("default", "", "/env/mux.sock", "/run/user/1000", TestUser) == "/env/mux.sock");
}

TEST_CASE("the XDG runtime dir hosts the per-label socket", "[vthost][socketpath]")
{
    CHECK(muxSocketPath("default", "", std::nullopt, "/run/user/1000", TestUser)
          == "/run/user/1000/contour/default");
    CHECK(muxSocketPath("work", "", std::nullopt, "/run/user/1000", TestUser)
          == "/run/user/1000/contour/work");
}

TEST_CASE("an empty env value counts as unset", "[vthost][socketpath]")
{
    CHECK(muxSocketPath("default", "", "", "/run/user/1000", TestUser) == "/run/user/1000/contour/default");
}

TEST_CASE("without a runtime dir the path falls back to a per-user temp dir", "[vthost][socketpath]")
{
    auto const path = muxSocketPath("fallback", "", std::nullopt, std::nullopt, TestUser);

    CHECK(path.parent_path().filename() == std::string { "contour-" } + TestUser);
    CHECK(path.filename() == "fallback"); // the label is the leaf, whatever the OS separator
}

TEST_CASE("the production overload reads its inputs from the injected environment", "[vthost][socketpath]")
{
    // What the `env` parameter is for: no variable of the test process is consulted, and none is
    // mutated to arrange this.
    auto const environment = crispy::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "/run/user/4242" } } };

    CHECK(muxSocketPath("work", "", environment) == "/run/user/4242/contour/work");

    SECTION("and $CONTOUR_MUX still outranks it")
    {
        auto const overridden = crispy::testing::FakeEnvironment { {
            { "CONTOUR_MUX", "/env/mux.sock" },
            { "XDG_RUNTIME_DIR", "/run/user/4242" },
        } };

        CHECK(muxSocketPath("work", "", overridden) == "/env/mux.sock");
    }
}
