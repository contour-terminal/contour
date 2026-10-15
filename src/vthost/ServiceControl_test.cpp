// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>
#include <tuple>

#include <vthost/ServiceControl.hpp>

using namespace vthost;

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
    CHECK(serviceNameForLabel("default") == "contour-daemon-default");
    CHECK(serviceNameForLabel("work") == "contour-daemon-work");
    CHECK(serviceNameForLabel("default") != serviceNameForLabel("work"));
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
        // `boot`/`manual` are reachable from the CLI but not built; the message is the only
        // thing standing between the user and a silent no-op.
        auto backend = makeServiceBackend(ServiceStartMode::Boot, "contour-daemon-test");
        REQUIRE(backend != nullptr);
        auto const status = backend->status();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ServiceErrorCode::Unsupported);

        // The category alone ("unsupported on this platform") leaves the user with nothing to
        // do next, so every Unsupported error carries context. What that context can offer
        // differs by platform: Windows has a working `logon` backend to redirect to, POSIX has
        // no backend at all yet and can only say so.
        auto const text = status.error().toString();
        CHECK_FALSE(status.error().context.empty());
#ifdef _WIN32
        CHECK(text.contains("--start=logon"));
#else
        CHECK(text.contains("no service backend"));
#endif
    }
}

TEST_CASE("a backend exists for every start mode", "[vthost][service]")
{
    // makeServiceBackend never returns null, so callers have ONE error path (the expected)
    // rather than a null check plus an expected — and the null branch is the one nobody writes
    // a message for.
    for (auto const& [text, mode]: ServiceStartModeNames)
    {
        auto backend = makeServiceBackend(mode, std::string { "contour-daemon-" } + std::string { text });
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

    SECTION("the registration name")
    {
        auto backend = makeServiceBackend(ServiceStartMode::Logon, std::format("contour-daemon-{}", NotUtf8));
        auto const status = backend->status();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ServiceErrorCode::NotUtf8);
        CHECK(status.error().context == "the registration name");
    }

    SECTION("the program")
    {
        auto backend = makeServiceBackend(ServiceStartMode::Logon, "contour-daemon-test-not-utf8");
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
