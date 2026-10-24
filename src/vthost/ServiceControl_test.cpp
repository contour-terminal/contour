// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <vthost/ServiceControl.hpp>
#include <vthost/testing/FakeCommandRunner.hpp>

using namespace vthost;

namespace
{
/// The host a backend reaches, replaced by fakes: nothing here touches the real init system.
struct FakeServices
{
    core::platform::testing::InMemoryFileSystem files {};
    vthost::testing::FakeCommandRunner commands {};
    core::testing::FakeEnvironment environment { { { "HOME", "/home/u" } } };

    [[nodiscard]] ServiceBackendServices services() { return { files, commands, environment }; }
};
} // namespace

TEST_CASE("every start mode round-trips through its CLI spelling", "[vthost][service]")
{
    // The table is what the CLI parses, what --start's help lists and what `status` prints, so a
    // mode whose spelling does not round-trip is one the user can name but not reach.
    for (auto const& [text, mode]: ServiceStartModeNames)
    {
        CHECK(serviceStartModeFrom(text) == mode);
        CHECK(nameOf(mode) == text);
    }
}

TEST_CASE("an unknown start mode is rejected rather than defaulted", "[vthost][service]")
{
    // Silently installing a logon task for someone who asked for `boot` would look like the
    // option does nothing.
    CHECK_FALSE(serviceStartModeFrom("").has_value());
    CHECK_FALSE(serviceStartModeFrom("Logon").has_value()); // case-sensitive, like every other verb
    CHECK_FALSE(serviceStartModeFrom("startup").has_value());
}

TEST_CASE("the registration name is derived from the socket label", "[vthost][service]")
{
    // Labels already distinguish daemon instances; without carrying that into the registration
    // name, installing a second labelled daemon would overwrite the first's registration.
    CHECK(serviceNameForLabel("default") == "contour-daemon");
    CHECK(serviceNameForLabel("work") == "contour-daemon-work");
    CHECK(serviceNameForLabel("default") != serviceNameForLabel("work"));
}

TEST_CASE("a registration is looked up under its current name, then its pre-phase-2 one", "[vthost][service]")
{
    // An existing Windows `contour-daemon-default` task must stay findable, stoppable and removable.
    CHECK(registrationNamesForLabel("default")
          == std::vector<std::string> { "contour-daemon", "contour-daemon-default" });
    CHECK(registrationNamesForLabel("work") == std::vector<std::string> { "contour-daemon-work" });
}

TEST_CASE("a service error reports its cause, not just that one occurred", "[vthost][service]")
{
    SECTION("the category alone when there is nothing to add")
    {
        auto const error = ServiceError { .code = ServiceErrorCode::NotInstalled };
        CHECK(error.toString() == "not installed");
    }

    SECTION("the failing call and the OS status when there are")
    {
        auto const error = ServiceError { .code = ServiceErrorCode::AccessDenied,
                                          .systemCode = 5,
                                          .context = "CreateServiceW" };
        auto const text = error.toString();
        CHECK(text.contains("access denied"));
        CHECK(text.contains("CreateServiceW"));
        CHECK(text.contains("5"));
    }

    SECTION("an unimplemented backend explains itself rather than refusing bare")
    {
        // `boot`/`manual` are reachable from the CLI but only Windows has them; the message is the only
        // thing standing between the user and a silent no-op.
        auto fake = FakeServices {};
        auto backend = makeServiceBackend(ServiceStartMode::Boot, "contour-daemon-test", fake.services());
        REQUIRE(backend != nullptr);
        auto const status = backend->status();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ServiceErrorCode::Unsupported);

        // The category alone ("unsupported on this platform") leaves the user with nothing to do next,
        // so every Unsupported error carries context that names the way forward.
        CHECK_FALSE(status.error().context.empty());
        CHECK(status.error().toString().contains("--start=logon"));
    }
}

TEST_CASE("a backend exists for every start mode", "[vthost][service]")
{
    // makeServiceBackend never returns null, so callers have ONE error path (the expected)
    // rather than a null check plus an expected — and the null branch is the one nobody writes
    // a message for.
    for (auto const& [text, mode]: ServiceStartModeNames)
    {
        auto fake = FakeServices {};
        auto backend = makeServiceBackend(
            mode, std::string { "contour-daemon-" } + std::string { text }, fake.services());
        CHECK(backend != nullptr);
    }
}

#ifdef _WIN32
TEST_CASE("a logon registration refuses text that is not UTF-8 before it asks the Task Scheduler",
          "[vthost][service]")
{
    // Spelled with U+FFFD instead, a name or a program that is not UTF-8 would look a task up under another
    // name, or register one that runs another file. Each is refused before the Task Scheduler is asked
    // anything, so this registers nothing.
    auto constexpr NotUtf8 = std::string_view { "J\xfcrgen" }; // as the ANSI code page spells "Jürgen"
    auto fake = FakeServices {};

    SECTION("the registration name")
    {
        auto backend = makeServiceBackend(
            ServiceStartMode::Logon, std::format("contour-daemon-{}", NotUtf8), fake.services());
        auto const status = backend->status();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ServiceErrorCode::NotUtf8);
        CHECK(status.error().context == "the registration name");
    }

    SECTION("the program")
    {
        auto backend =
            makeServiceBackend(ServiceStartMode::Logon, "contour-daemon-test-not-utf8", fake.services());
        auto const installed = backend->install(ServiceInstallRequest {
            .commandLine = { std::format(R"(C:\Users\{}\contour.exe)", NotUtf8), "daemon" },
            .mode = ServiceStartMode::Logon,
            .displayName = "contour test",
            .description = "registered by mistake by vthost_test" });
        if (installed)
            std::ignore = backend->uninstall(); // registered after all: take it off this account again
        REQUIRE_FALSE(installed.has_value());
        CHECK(installed.error().toString() == "not UTF-8 (the program)");
    }
}
#endif

#ifdef __linux__
TEST_CASE("on Linux a logon registration is a systemd user unit", "[vthost][service][systemd]")
{
    auto fake = FakeServices {};
    auto backend = makeServiceBackend(ServiceStartMode::Logon, "contour-daemon", fake.services());
    REQUIRE(backend != nullptr);
    std::ignore = backend->start();
    REQUIRE(fake.commands.calls.size() == 1);
    CHECK(fake.commands.calls.front()
          == std::vector<std::string> { "systemctl", "--user", "start", "contour-daemon.service" });
    CHECK(backend->launchStyle() == DaemonLaunchStyle::Foreground);
}

TEST_CASE("on Linux without a home directory the backend refuses and says why", "[vthost][service][systemd]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto commands = vthost::testing::FakeCommandRunner {};
    auto const environment = core::testing::FakeEnvironment {};
    auto backend =
        makeServiceBackend(ServiceStartMode::Logon, "contour-daemon", { files, commands, environment });
    auto const status = backend->status();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ServiceErrorCode::Unsupported);
    CHECK(status.error().toString().contains("HOME is not set"));
    CHECK(commands.calls.empty());
}
#endif
