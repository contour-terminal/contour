// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <vthost/ServiceControl.h>

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
