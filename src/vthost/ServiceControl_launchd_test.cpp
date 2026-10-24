// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

#include <vthost/ServiceControl_launchd.hpp>
#include <vthost/testing/FakeCommandRunner.hpp>

using Calls = std::vector<std::vector<std::string>>;

namespace
{
constexpr auto Label = "org.contourterminal.contour-daemon";
constexpr auto PlistPath = "/Users/u/Library/LaunchAgents/org.contourterminal.contour-daemon.plist";
constexpr auto Target = "gui/501/org.contourterminal.contour-daemon";

struct Fixture
{
    core::platform::testing::InMemoryFileSystem files {};
    vthost::testing::FakeCommandRunner commands {};
    core::testing::FakeEnvironment environment {};
    vthost::LaunchdBackend backend { { .label = Label, .plistPath = PlistPath, .domain = "gui/501" },
                                     { files, commands, environment } };

    [[nodiscard]] static vthost::ServiceInstallRequest request()
    {
        return { .commandLine = {
                     "/Applications/Contour.app/Contents/MacOS/contour", "daemon", "--socket=/a&b" } };
    }
};
} // namespace

TEST_CASE("launchd names derive from the registration name", "[vthost][service][launchd]")
{
    CHECK(vthost::launchdLabelFor("contour-daemon") == "org.contourterminal.contour-daemon");
    CHECK(vthost::launchAgentPath("/Users/u", Label) == std::filesystem::path { PlistPath });
    CHECK(vthost::launchdGuiDomain(501) == "gui/501");
}

TEST_CASE("the LaunchAgent plist runs the request and restarts only after a crash",
          "[vthost][service][launchd]")
{
    auto const plist = vthost::renderLaunchAgentPlist(Label, Fixture::request());
    CHECK(plist.contains("<key>Label</key>\n    <string>org.contourterminal.contour-daemon</string>"));
    CHECK(plist.contains("<string>--socket=/a&amp;b</string>"));
    CHECK(plist.contains("<key>SuccessfulExit</key>\n        <false/>"));
    CHECK(plist.contains("<key>RunAtLoad</key>\n    <true/>"));
    CHECK(plist.contains("<key>ProcessType</key>\n    <string>Interactive</string>"));
}

TEST_CASE("plist text is XML-escaped and its arguments read back", "[vthost][service][launchd]")
{
    auto const request =
        vthost::ServiceInstallRequest { .commandLine = { "/p/<c>", "a&b", "\"q\"", "it's" } };
    auto const plist = vthost::renderLaunchAgentPlist(Label, request);
    CHECK(plist.contains("&lt;c&gt;"));
    CHECK(plist.contains("&quot;q&quot;"));
    CHECK(plist.contains("it&apos;s"));
    CHECK(vthost::parseLaunchAgentArguments(plist) == request.commandLine);
}

TEST_CASE("launchctl print maps onto run states", "[vthost][service][launchd]")
{
    CHECK(vthost::parseLaunchctlPrint(0, "\tstate = running\n\tpid = 42\n")
          == vthost::ServiceRunState::Running);
    CHECK(vthost::parseLaunchctlPrint(0, "\tstate = waiting\n") == vthost::ServiceRunState::Stopped);
    CHECK(vthost::parseLaunchctlPrint(113, "Could not find service") == vthost::ServiceRunState::Stopped);
    CHECK(vthost::parseLaunchctlPrint(113, "\tstate = running\n") == vthost::ServiceRunState::Stopped);
}

TEST_CASE("install writes the plist and loads nothing", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    CHECK(fixture.files.exists(PlistPath));
    CHECK(fixture.commands.calls.empty());
    CHECK(fixture.backend.launchStyle() == vthost::DaemonLaunchStyle::Foreground);
}

TEST_CASE("start bootstraps, and kickstarts an already-loaded agent", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    fixture.commands.responder =
        [](std::span<std::string const> argv) -> std::expected<vthost::CommandOutcome, std::error_code> {
        return vthost::CommandOutcome { .exitCode = argv[1] == "bootstrap" ? 37 : 0 };
    };
    REQUIRE(fixture.backend.start());
    CHECK(fixture.commands.calls
          == Calls { { "launchctl", "bootstrap", "gui/501", PlistPath },
                     { "launchctl", "kickstart", Target } });
}

TEST_CASE("start without a plist is NotInstalled", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    auto const result = fixture.backend.start();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == vthost::ServiceErrorCode::NotInstalled);
    CHECK(fixture.commands.calls.empty());
}

TEST_CASE("stop sends SIGTERM to the loaded agent", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.stop());
    CHECK(fixture.commands.calls == Calls { { "launchctl", "kill", "SIGTERM", Target } });
}

TEST_CASE("a failing launchctl is a Backend error naming the verb", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    fixture.commands.answerAll({ .exitCode = 113 });
    auto const result = fixture.backend.stop();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == vthost::ServiceErrorCode::Backend);
    CHECK(result.error().systemCode == 113);
    CHECK(result.error().context == "launchctl kill");
}

TEST_CASE("a missing launchctl is reported", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    fixture.commands.failToStart(std::make_error_code(std::errc::no_such_file_or_directory));
    auto const result = fixture.backend.stop();
    REQUIRE_FALSE(result);
    CHECK(result.error().toString().contains("launchctl not found"));
}

TEST_CASE("uninstall removes the plist even when the agent was not loaded", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    fixture.commands.answerAll({ .exitCode = 113 });

    REQUIRE(fixture.backend.uninstall());
    CHECK_FALSE(fixture.files.exists(PlistPath));
    CHECK(fixture.commands.calls == Calls { { "launchctl", "bootout", Target } });

    auto const again = fixture.backend.uninstall();
    REQUIRE_FALSE(again);
    CHECK(again.error().code == vthost::ServiceErrorCode::NotInstalled);
}

TEST_CASE("status reads the plist and launchctl print", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};

    SECTION("no plist is not installed")
    {
        CHECK(fixture.backend.status().value().state == vthost::ServiceRunState::NotInstalled);
        CHECK(fixture.commands.calls.empty());
    }

    SECTION("a loaded, running agent")
    {
        REQUIRE(fixture.backend.install(Fixture::request()));
        fixture.commands.answerAll({ .exitCode = 0, .standardOutput = "\tstate = running\n" });
        auto const status = fixture.backend.status().value();
        CHECK(status.state == vthost::ServiceRunState::Running);
        CHECK(status.commandLine == "/Applications/Contour.app/Contents/MacOS/contour daemon --socket=/a&b");
        CHECK(fixture.commands.calls == Calls { { "launchctl", "print", Target } });
    }

    SECTION("a registered agent that is not loaded is stopped")
    {
        REQUIRE(fixture.backend.install(Fixture::request()));
        fixture.commands.answerAll({ .exitCode = 113 });
        CHECK(fixture.backend.status().value().state == vthost::ServiceRunState::Stopped);
    }
}

TEST_CASE("start reports the bootstrap failure when kickstart fails too", "[vthost][service][launchd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    fixture.commands.responder =
        [](std::span<std::string const> argv) -> std::expected<vthost::CommandOutcome, std::error_code> {
        return vthost::CommandOutcome { .exitCode = argv[1] == "bootstrap" ? 5 : 113 };
    };
    auto const result = fixture.backend.start();
    REQUIRE_FALSE(result);
    CHECK(result.error().systemCode == 5);
    CHECK(result.error().context == "launchctl bootstrap");
}
