// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemInfo.hpp>

#include <catch2/catch_test_macros.hpp>

using core::platform::cachedHostName;
using core::platform::hostName;

TEST_CASE("cachedHostName resolves the host name once and keeps it", "[platform]")
{
    auto const& first = cachedHostName();
    auto const& second = cachedHostName();
    CHECK(&first == &second);
    CHECK(first == hostName());
}
