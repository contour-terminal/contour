// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <vthost/DaemonSpawn.hpp>
#include <vthost/ServiceControl_systemd.hpp>
#include <vthost/testing/FakeCommandRunner.hpp>

using namespace std::string_literals;
using Calls = std::vector<std::vector<std::string>>;

namespace
{
constexpr auto UnitName = "contour-daemon.service";
constexpr auto UnitPath = "/home/u/.config/systemd/user/contour-daemon.service";

/// Everything a SystemdUserBackend test needs, wired as production wires it but over fakes.
struct Fixture
{
    core::platform::testing::InMemoryFileSystem files {};
    vthost::testing::FakeCommandRunner commands {};
    core::testing::FakeEnvironment environment { { { "HOME", "/home/u" } } };
    vthost::SystemdUserBackend backend { { .unitName = UnitName, .unitPath = UnitPath },
                                         { files, commands, environment } };

    [[nodiscard]] static vthost::ServiceInstallRequest request()
    {
        return { .commandLine = { "/usr/bin/contour", "daemon" }, .displayName = "Contour daemon (default)" };
    }
};
} // namespace

TEST_CASE("systemd quoting survives spaces, quotes, percent and dollar", "[vthost][service][systemd]")
{
    CHECK(vthost::systemdQuote("/usr/bin/contour") == R"("/usr/bin/contour")");
    CHECK(vthost::systemdQuote(R"(a "b" c\d)") == R"("a \"b\" c\\d")");
    CHECK(vthost::systemdQuote("50%$HOME") == R"("50%%$$HOME")");
}

TEST_CASE("the rendered unit runs exactly the requested command line", "[vthost][service][systemd]")
{
    auto const unit = vthost::renderSystemdUnit(
        { .commandLine = { "/usr/bin/contour", "daemon", "--socket=/run/user/1000/contour/default" },
          .displayName = "Contour daemon (default)" });
    CHECK(unit.contains("Description=Contour daemon (default)\n"));
    CHECK(
        unit.contains(R"(ExecStart="/usr/bin/contour" "daemon" "--socket=/run/user/1000/contour/default")"));
    CHECK(unit.contains("WantedBy=default.target\n"));
    CHECK(unit.contains("Restart=on-failure\n"));
    CHECK(unit.contains("Documentation=https://contour-terminal.org/persistent-sessions/\n"));
}

TEST_CASE("a socket path the unit escapes is read back from the status it reports",
          "[vthost][service][systemd]")
{
    // Every character systemdQuote() escapes, so the reader must undo each escape the writer made.
    auto const socket = GENERATE(as<std::string> {},
                                 "/run/user/1000/contour/plain",
                                 "/run/user/1000/50%/a b",
                                 R"(/run/user/1000/back\slash)",
                                 R"(/run/user/1000/"quoted"/x)",
                                 "/run/user/1000/$HOME/%%",
                                 R"(/run/user/1000/all \"%$\)");
    auto fixture = Fixture {};
    REQUIRE(fixture.files.writeFile(
        UnitPath,
        vthost::renderSystemdUnit({ .commandLine = { "/usr/bin/contour", "daemon", "--socket=" + socket },
                                    .displayName = "Contour daemon (default)" })));
    fixture.commands.answerAll({ .exitCode = 0, .standardOutput = "LoadState=loaded\nActiveState=active\n" });
    auto const status = fixture.backend.status().value();
    CHECK(vthost::registeredSocketPath(status.commandLine) == std::filesystem::path { socket });
}

TEST_CASE("the user unit directory follows XDG_CONFIG_HOME, else the home", "[vthost][service][systemd]")
{
    using core::testing::FakeEnvironment;
    CHECK(vthost::systemdUserUnitDirectory(FakeEnvironment { { { "HOME", "/home/u" } } })
          == std::filesystem::path { "/home/u/.config/systemd/user" });
    CHECK(vthost::systemdUserUnitDirectory(
              FakeEnvironment { { { "HOME", "/home/u" }, { "XDG_CONFIG_HOME", "/cfg" } } })
          == std::filesystem::path { "/cfg/systemd/user" });
    CHECK(vthost::systemdUserUnitDirectory(FakeEnvironment {}).empty());
}

TEST_CASE("systemctl show output parses into key/value pairs", "[vthost][service][systemd]")
{
    auto const parsed = vthost::parseSystemctlShow("LoadState=loaded\nActiveState=active\nnoise\nEmpty=\n");
    CHECK(parsed.at("LoadState") == "loaded");
    CHECK(parsed.at("ActiveState") == "active");
    CHECK(parsed.at("Empty").empty());
    CHECK(parsed.size() == 3);
}

TEST_CASE("install writes the unit, reloads and enables it", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    CHECK(fixture.files.exists(UnitPath));
    CHECK(fixture.files.readFile(UnitPath).value().contains(R"(ExecStart="/usr/bin/contour" "daemon")"));
    CHECK(fixture.commands.calls
          == Calls { { "systemctl", "--user", "daemon-reload" },
                     { "systemctl", "--user", "enable", UnitName } });
}

TEST_CASE("installing again rewrites the unit", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    auto request = Fixture::request();
    request.commandLine.emplace_back("--log=x");
    REQUIRE(fixture.backend.install(request));
    CHECK(fixture.files.readFile(UnitPath).value().contains(R"("--log=x")"));
}

TEST_CASE("start and stop address the unit", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.start());
    REQUIRE(fixture.backend.stop());
    CHECK(fixture.commands.calls
          == Calls { { "systemctl", "--user", "start", UnitName },
                     { "systemctl", "--user", "stop", UnitName } });
}

TEST_CASE("a failing systemctl is a Backend error naming the verb and carrying the exit code",
          "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    fixture.commands.answerAll({ .exitCode = 5 });
    auto const result = fixture.backend.start();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == vthost::ServiceErrorCode::Backend);
    CHECK(result.error().systemCode == 5);
    CHECK(result.error().context == "systemctl --user start");
}

TEST_CASE("launched by systemd, the daemon stays in the foreground", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    CHECK(fixture.backend.launchStyle() == vthost::DaemonLaunchStyle::Foreground);
}

TEST_CASE("status maps systemctl show onto run states", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    auto const answer = [&](std::string text) {
        fixture.commands.answerAll({ .exitCode = 0, .standardOutput = std::move(text) });
    };

    SECTION("an unknown unit is not installed")
    {
        answer("LoadState=not-found\nActiveState=inactive\n");
        CHECK(fixture.backend.status().value().state == vthost::ServiceRunState::NotInstalled);
        CHECK(fixture.commands.calls.back()
              == std::vector<std::string> {
                  "systemctl", "--user", "show", UnitName, "-p", "LoadState", "-p", "ActiveState" });
    }

    SECTION("an active unit is running and reports the command from its unit file")
    {
        REQUIRE(fixture.files.writeFile(UnitPath, vthost::renderSystemdUnit(Fixture::request())));
        answer("LoadState=loaded\nActiveState=active\n");
        auto const status = fixture.backend.status().value();
        CHECK(status.state == vthost::ServiceRunState::Running);
        CHECK(status.commandLine == R"("/usr/bin/contour" "daemon")");
    }

    SECTION("a failed unit is stopped")
    {
        answer("LoadState=loaded\nActiveState=failed\n");
        CHECK(fixture.backend.status().value().state == vthost::ServiceRunState::Stopped);
    }

    SECTION("a non-zero exit is a Backend error naming systemctl show")
    {
        fixture.commands.answerAll({ .exitCode = 1 });
        auto const status = fixture.backend.status();
        REQUIRE_FALSE(status);
        CHECK(status.error().code == vthost::ServiceErrorCode::Backend);
        CHECK(status.error().context == "systemctl --user show");
    }
}

TEST_CASE("uninstall disables the unit, removes the file and reloads", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.backend.install(Fixture::request()));
    fixture.commands.calls.clear();
    fixture.commands.responder =
        [](std::span<std::string const> argv) -> std::expected<vthost::CommandOutcome, std::error_code> {
        // `disable --now` of an already-disabled unit may fail; uninstall must not care.
        return vthost::CommandOutcome { .exitCode = argv[2] == "disable" ? 1 : 0,
                                        .standardOutput = "LoadState=loaded\nActiveState=inactive\n" };
    };

    REQUIRE(fixture.backend.uninstall());
    CHECK_FALSE(fixture.files.exists(UnitPath));
    CHECK(fixture.commands.calls.back()
          == std::vector<std::string> { "systemctl", "--user", "daemon-reload" });
    CHECK(std::ranges::contains(
        fixture.commands.calls,
        std::vector<std::string> { "systemctl", "--user", "disable", "--now", UnitName }));
}

TEST_CASE("uninstalling what is neither a file nor a loaded unit is NotInstalled",
          "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    fixture.commands.answerAll(
        { .exitCode = 0, .standardOutput = "LoadState=not-found\nActiveState=inactive\n" });
    auto const result = fixture.backend.uninstall();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == vthost::ServiceErrorCode::NotInstalled);
}

TEST_CASE("a missing systemctl is reported, not crashed on", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    fixture.commands.failToStart(std::make_error_code(std::errc::no_such_file_or_directory));
    auto const result = fixture.backend.install(Fixture::request());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == vthost::ServiceErrorCode::Backend);
    CHECK(result.error().toString().contains("systemctl"));
}

TEST_CASE("a display name cannot inject unit lines or specifiers", "[vthost][service][systemd]")
{
    auto const unit = vthost::renderSystemdUnit(
        { .commandLine = { "/usr/bin/contour" }, .displayName = "100%\nExecStart=/bin/evil\r" });
    CHECK(unit.contains("Description=100%%ExecStart=/bin/evil\n"));
    CHECK_FALSE(unit.contains("\nExecStart=/bin/evil"));
}

TEST_CASE("uninstall with a missing systemctl reports that, not NotInstalled", "[vthost][service][systemd]")
{
    auto fixture = Fixture {};
    fixture.commands.failToStart(std::make_error_code(std::errc::no_such_file_or_directory));
    auto const result = fixture.backend.uninstall();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == vthost::ServiceErrorCode::Backend);
    CHECK(result.error().toString().contains("systemctl"));
}
