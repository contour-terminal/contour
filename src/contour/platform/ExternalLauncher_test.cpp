// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the launch-failure vocabulary — the reason an open request carries when it is
// rejected without ever reaching the desktop. Pure and constexpr: the point of the enum is that
// every cause has a sentence, and a new enumerator must not silently fall through to "unknown".
// @see issue #2075.

#include <contour/platform/ExternalLauncher.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::platform::describe;
using contour::platform::LaunchError;

TEST_CASE("describe(LaunchError) names every cause", "[contour][launcher]")
{
    // Each enumerator listed by hand rather than swept: the assertion worth making is that a value
    // added tomorrow has a sentence written for it, and a loop over a range would pass without one.
    STATIC_CHECK(describe(LaunchError::InvalidUrl) == "the address is not a valid URL");
    STATIC_CHECK(describe(LaunchError::NoHandler) == "no application is registered to open it");
    STATIC_CHECK(describe(LaunchError::DispatchFailed) == "the desktop could not be asked to open it");
}

TEST_CASE("describe(LaunchError) never returns the fallback for a named cause", "[contour][launcher]")
{
    // The switch ends in a `return "unknown error"` that only an out-of-range value can reach. If a
    // future enumerator is added without a case, it lands here -- so this pins that none does today.
    STATIC_CHECK(describe(LaunchError::InvalidUrl) != "unknown error");
    STATIC_CHECK(describe(LaunchError::NoHandler) != "unknown error");
    STATIC_CHECK(describe(LaunchError::DispatchFailed) != "unknown error");
}
