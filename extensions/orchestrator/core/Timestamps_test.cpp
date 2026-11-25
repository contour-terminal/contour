// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <orchestrator/core/Timestamps.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

TEST_CASE("unixMsOf reads the three forges' spellings", "[orchestrator][timestamps]")
{
    CHECK(unixMsOf("2026-11-13T15:08:47Z") == 1'794'582'527'000);
    CHECK(unixMsOf("2026-11-13T16:08:47.250+01:00") == 1'794'582'527'250);
    CHECK(unixMsOf("2026-11-13T16:08:47.000+0100") == 1'794'582'527'000);
    CHECK(unixMsOf("2026-11-13T12:00:00.123456Z") == 1'794'571'200'123);
    // Past 2038-01-19, where a 32-bit second count overflows; the last representable date in the four-digit
    // years.
    CHECK(unixMsOf("2038-01-19T03:14:08Z") == 2'147'483'648'000);
    CHECK(unixMsOf("2038-01-20T00:00:00Z") == 2'147'558'400'000);
    CHECK(unixMsOf("9999-12-31T23:59:59Z") == 253'402'300'799'000);
    for (auto const bad: { ""sv,
                           "2026-11-13T15:08:47"sv,
                           "2026-11-13 15:08:47Z"sv,
                           "2026-13-01T00:00:00Z"sv,
                           "2026-02-30T00:00:00Z"sv,
                           "1969-12-31T23:59:59Z"sv,
                           "2026-11-13T15:08:47+1"sv })
    {
        CAPTURE(bad);
        CHECK_FALSE(unixMsOf(bad).has_value());
    }
}
