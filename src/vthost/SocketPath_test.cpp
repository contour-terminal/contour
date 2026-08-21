// SPDX-License-Identifier: Apache-2.0
#include <crispy/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <vthost/SocketPath.hpp>

using vthost::muxSocketPath;
using vthost::MuxSocketPathInputs;

namespace
{
/// Stands in for whatever identifies the user on the host running the suite, so the fallback
/// branch's expectations are exact rather than "contains something".
constexpr auto TestUser = "1000";

/// A sandboxed application's own id, as /.flatpak-info reports it. Off a sandbox the field is
/// simply left out, which is what makes the designated-initializer form worth the struct.
constexpr auto SandboxedAs = "org.contourterminal.Contour";
} // namespace

TEST_CASE("an explicit --socket path wins over everything", "[vthost][socketpath]")
{
    CHECK(muxSocketPath(MuxSocketPathInputs { .explicitPath = "/tmp/explicit.sock",
                                              .contourMuxEnv = "/env/mux",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .user = TestUser })
          == "/tmp/explicit.sock");
}

TEST_CASE("$CONTOUR_MUX overrides the derived path", "[vthost][socketpath]")
{
    CHECK(muxSocketPath(MuxSocketPathInputs {
              .contourMuxEnv = "/env/mux.sock", .xdgRuntimeDir = "/run/user/1000", .user = TestUser })
          == "/env/mux.sock");
}

TEST_CASE("the XDG runtime dir hosts the per-label socket", "[vthost][socketpath]")
{
    CHECK(muxSocketPath(MuxSocketPathInputs { .xdgRuntimeDir = "/run/user/1000", .user = TestUser })
          == "/run/user/1000/contour/default");
    CHECK(muxSocketPath(
              MuxSocketPathInputs { .label = "work", .xdgRuntimeDir = "/run/user/1000", .user = TestUser })
          == "/run/user/1000/contour/work");
}

TEST_CASE("a sandboxed derivation lands where both sides of the sandbox can see it", "[vthost][socketpath]")
{
    // Inside a Flatpak, XDG_RUNTIME_DIR is a directory created in the sandbox's OWN mount namespace,
    // so a socket bound directly under it is unreachable from the host at any path -- and a shell
    // that escaped the sandbox is exactly what runs `contour client`. $XDG_RUNTIME_DIR/app/<id> is
    // the one place flatpak shares between the two, at the same absolute path in both.
    // @see issue #2075.
    CHECK(muxSocketPath(MuxSocketPathInputs {
              .xdgRuntimeDir = "/run/user/1000", .sandboxAppId = SandboxedAs, .user = TestUser })
          == "/run/user/1000/app/org.contourterminal.Contour/contour/default");
    CHECK(muxSocketPath(MuxSocketPathInputs { .label = "work",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .sandboxAppId = SandboxedAs,
                                              .user = TestUser })
          == "/run/user/1000/app/org.contourterminal.Contour/contour/work");
}

TEST_CASE("the sandbox rung does not outrank an explicit path or $CONTOUR_MUX", "[vthost][socketpath]")
{
    // Both remain the escape hatch for the case the rung cannot fix: a daemon on the HOST is not
    // sandboxed, so it derives the plain path, and only naming the socket makes the two agree.
    CHECK(muxSocketPath(MuxSocketPathInputs { .explicitPath = "/tmp/explicit.sock",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .sandboxAppId = SandboxedAs,
                                              .user = TestUser })
          == "/tmp/explicit.sock");
    CHECK(muxSocketPath(MuxSocketPathInputs { .contourMuxEnv = "/env/mux.sock",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .sandboxAppId = SandboxedAs,
                                              .user = TestUser })
          == "/env/mux.sock");
}

TEST_CASE("without a runtime dir the sandbox rung has nothing to qualify", "[vthost][socketpath]")
{
    // The app-scoped directory only exists BENEATH a runtime dir, so with none there is nothing to
    // qualify and the temp fallback stands as it does off the sandbox.
    auto const path = muxSocketPath(
        MuxSocketPathInputs { .label = "fallback", .sandboxAppId = SandboxedAs, .user = TestUser });

    CHECK(path.parent_path().filename() == std::string { "contour-" } + TestUser);
    CHECK(path.filename() == "fallback");
}

TEST_CASE("an empty env value counts as unset", "[vthost][socketpath]")
{
    CHECK(muxSocketPath(MuxSocketPathInputs {
              .contourMuxEnv = "", .xdgRuntimeDir = "/run/user/1000", .user = TestUser })
          == "/run/user/1000/contour/default");
}

TEST_CASE("without a runtime dir the path falls back to a per-user temp dir", "[vthost][socketpath]")
{
    auto const path = muxSocketPath(MuxSocketPathInputs { .label = "fallback", .user = TestUser });

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

TEST_CASE("the production overload takes the sandbox from where it is injected too", "[vthost][socketpath]")
{
    // The sandbox is a parameter for the same reason the environment is: without one, this case
    // could only be reached by running the suite inside an actual Flatpak.
    auto const environment = crispy::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "/run/user/4242" } } };
    auto const sandboxed = vtpty::SandboxInfo { .state = vtpty::SandboxState::Flatpak,
                                                .network = vtpty::NetworkAccess::Denied,
                                                .applicationId = SandboxedAs };

    CHECK(muxSocketPath("work", "", environment, sandboxed)
          == "/run/user/4242/app/org.contourterminal.Contour/contour/work");
}

TEST_CASE("a daemon-hosted shell is told where its own daemon is", "[vthost][socketpath]")
{
    // It cannot derive it: with escape_sandbox the shell runs on the HOST while the daemon that
    // spawned it does not, and the two sides derive different paths. @see issue #2075.
    auto const environment = vthost::hostedShellEnvironment("/run/user/1000/contour/default");

    REQUIRE(environment.contains("CONTOUR_MUX"));
    CHECK(environment.at("CONTOUR_MUX") == "/run/user/1000/contour/default");
}
