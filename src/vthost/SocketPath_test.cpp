// SPDX-License-Identifier: Apache-2.0
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/HostedShell.hpp>
#include <vthost/SocketPath.hpp>

using vthost::LabelOrigin;
using vthost::muxSocketPath;
using vthost::MuxSocketPathInputs;
using vthost::SocketLabel;

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

TEST_CASE("the socket path is read as UTF-8, as the command line and the environment are",
          "[vthost][socketpath]")
{
    constexpr auto Awkward = vtpty::testing::NonAsciiName;
    auto const awkward = std::string { vtpty::testing::NonAsciiNameUtf8 };

    auto const explicitPath = "/tmp/" + awkward;
    CHECK(muxSocketPath(MuxSocketPathInputs { .explicitPath = explicitPath, .user = TestUser })
          == std::filesystem::path { u8"/tmp/" } / Awkward);

    CHECK(muxSocketPath(MuxSocketPathInputs { .label = awkward, .user = awkward })
          == std::filesystem::temp_directory_path()
                 / (std::u8string { u8"contour-" } + std::u8string { Awkward }) / Awkward);
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

TEST_CASE("a relative runtime dir counts as unset", "[vthost][socketpath]")
{
    // The XDG base directory specification has a relative value ignored: it would put the socket
    // beneath whatever directory the daemon or the client happened to start in, so the two would not
    // meet.
    auto const path = muxSocketPath(
        MuxSocketPathInputs { .label = "fallback", .xdgRuntimeDir = "run/user", .user = TestUser });

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
    auto const environment = core::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "/run/user/4242" } } };

    CHECK(muxSocketPath(vthost::socketLabelOf("work"), "", environment) == "/run/user/4242/contour/work");

    SECTION("and $CONTOUR_MUX still outranks it")
    {
        auto const overridden = core::testing::FakeEnvironment { {
            { "CONTOUR_MUX", "/env/mux.sock" },
            { "XDG_RUNTIME_DIR", "/run/user/4242" },
        } };

        CHECK(muxSocketPath(SocketLabel { .name = "work" }, "", overridden) == "/env/mux.sock");
    }
}

TEST_CASE("a label's own socket is what neither --socket nor $CONTOUR_MUX names", "[vthost][socketpath]")
{
    // What a daemon at a socket of its own is told apart from: where its label would serve otherwise.
    auto const overridden = core::testing::FakeEnvironment { {
        { "CONTOUR_MUX", "/env/mux.sock" },
        { "XDG_RUNTIME_DIR", "/run/user/4242" },
    } };
    CHECK(vthost::labelSocketPath("work", overridden) == "/run/user/4242/contour/work");
    CHECK(vthost::labelSocketPath(vthost::DefaultSocketLabel, overridden)
          == "/run/user/4242/contour/default");
    CHECK(muxSocketPath(SocketLabel { .name = "work" }, "", overridden) == "/env/mux.sock");
}

TEST_CASE("the production overload takes the sandbox from where it is injected too", "[vthost][socketpath]")
{
    // The sandbox is a parameter for the same reason the environment is: without one, this case
    // could only be reached by running the suite inside an actual Flatpak.
    auto const environment = core::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "/run/user/4242" } } };
    auto const sandboxed = vtpty::SandboxInfo { .state = vtpty::SandboxState::Flatpak,
                                                .network = vtpty::NetworkAccess::Denied,
                                                .applicationId = SandboxedAs };

    CHECK(muxSocketPath(vthost::socketLabelOf("work"), "", environment, sandboxed)
          == "/run/user/4242/app/org.contourterminal.Contour/contour/work");
}

TEST_CASE("a daemon-hosted shell is told where its own daemon is", "[vthost][socketpath]")
{
    // It cannot derive it: with escape_sandbox the shell runs on the HOST while the daemon that
    // spawned it does not, and the two sides derive different paths. @see issue #2075.
    auto const environment = vthost::hostedShellEnvironment("/run/user/1000/contour/default");

    REQUIRE(environment.contains("CONTOUR_MUX"));
    CHECK(environment.at("CONTOUR_MUX") == "/run/user/1000/contour/default");
    // The same path under the name the control API's clients read.
    REQUIRE(environment.contains("CONTOUR_DAEMON"));
    CHECK(environment.at("CONTOUR_DAEMON") == "/run/user/1000/contour/default");

    // In UTF-8, as the spawn reads an environment value.
    auto const awkward =
        vthost::hostedShellEnvironment(std::filesystem::path { u8"/tmp/" } / vtpty::testing::NonAsciiName);
    CHECK(awkward.at("CONTOUR_MUX") == std::format("/tmp/{}", vtpty::testing::NonAsciiNameUtf8));
    CHECK(awkward.at("CONTOUR_DAEMON") == awkward.at("CONTOUR_MUX"));
}

TEST_CASE("a hosted shell's pane variables are full references", "[vthost][socketpath]")
{
    auto const uuid = *vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    auto const environment = vthost::paneEnvironment(uuid, uuid, uuid);
    CHECK(environment.at("CONTOUR_PANE") == "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(environment.at("CONTOUR_TAB") == "tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(environment.at("CONTOUR_WINDOW") == "window:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(environment.size() == 3);

    // The daemon's own variables win over a profile's, the pane's ids among a profile's variables.
    auto base = vtpty::Process::ExecInfo {};
    base.program = "/bin/zsh";
    auto profile = vtpty::Process::ExecInfo {};
    profile.env = { { "CONTOUR_PANE", "bogus" }, { "A", "1" } };
    auto const shell = vthost::hostedShellFor(base, profile, vthost::ShellOverride::Replaces, environment);
    CHECK(shell.env.at("CONTOUR_PANE") == "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(shell.env.at("A") == "1");
}

TEST_CASE("an explicit label beats $CONTOUR_MUX, the default label yields to it", "[vthost][socketpath]")
{
    constexpr auto Mux = "/run/user/1000/contour/default";

    CHECK(muxSocketPath(MuxSocketPathInputs { .label = "dragtest",
                                              .labelOrigin = LabelOrigin::Explicit,
                                              .contourMuxEnv = Mux,
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .user = TestUser })
          == "/run/user/1000/contour/dragtest");

    // Nobody named a label: the daemon hosting the calling shell is the one meant.
    CHECK(muxSocketPath(MuxSocketPathInputs { .label = "dragtest",
                                              .labelOrigin = LabelOrigin::Default,
                                              .contourMuxEnv = "/env/mux.sock",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .user = TestUser })
          == "/env/mux.sock");
}

TEST_CASE("an explicitly given `default` label beats $CONTOUR_MUX too", "[vthost][socketpath]")
{
    CHECK(muxSocketPath(MuxSocketPathInputs { .label = vthost::DefaultSocketLabel,
                                              .labelOrigin = LabelOrigin::Explicit,
                                              .contourMuxEnv = "/env/mux.sock",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .user = TestUser })
          == "/run/user/1000/contour/default");
}

TEST_CASE("--socket beats both an explicit label and $CONTOUR_MUX", "[vthost][socketpath]")
{
    CHECK(muxSocketPath(MuxSocketPathInputs { .label = "dragtest",
                                              .labelOrigin = LabelOrigin::Explicit,
                                              .explicitPath = "/tmp/explicit.sock",
                                              .contourMuxEnv = "/env/mux.sock",
                                              .xdgRuntimeDir = "/run/user/1000",
                                              .user = TestUser })
          == "/tmp/explicit.sock");
}

TEST_CASE(
    "an explicit label is derived through the sandbox rung, and the unlabelled shell still finds its daemon",
    "[vthost][socketpath]")
{
    // The Flatpak rationale for $CONTOUR_MUX: a sandboxed shell with no --label reaches its own daemon.
    auto const environment = core::testing::FakeEnvironment { {
        { "CONTOUR_MUX", "/run/user/4242/app/org.contourterminal.Contour/contour/default" },
        { "XDG_RUNTIME_DIR", "/run/user/4242" },
    } };
    auto const sandboxed = vtpty::SandboxInfo { .state = vtpty::SandboxState::Flatpak,
                                                .network = vtpty::NetworkAccess::Denied,
                                                .applicationId = SandboxedAs };

    CHECK(muxSocketPath(SocketLabel {}, "", environment, sandboxed)
          == "/run/user/4242/app/org.contourterminal.Contour/contour/default");
    CHECK(muxSocketPath(vthost::socketLabelOf("work"), "", environment, sandboxed)
          == "/run/user/4242/app/org.contourterminal.Contour/contour/work");
}

TEST_CASE("socketLabelOf reads an empty option as not given", "[vthost][socketpath]")
{
    CHECK(vthost::socketLabelOf("").origin == LabelOrigin::Default);
    CHECK(vthost::socketLabelOf("").name == vthost::DefaultSocketLabel);
    CHECK(vthost::socketLabelOf("default").origin == LabelOrigin::Explicit);
    CHECK(vthost::socketLabelOf("work").name == "work");
}
