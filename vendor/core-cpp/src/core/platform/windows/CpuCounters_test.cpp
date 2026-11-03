// SPDX-License-Identifier: Apache-2.0
//
// Windows offers one counter without privileges: the cycles QueryProcessCycleTime() accounts to a
// process. This pins that down, so a backend that silently fell back to "unavailable" cannot pass.
#include <core/platform/CpuCounters.hpp>

#include <catch2/catch_test_macros.hpp>

using core::platform::CpuCounter;
using core::platform::makeDefaultCpuCounterSource;

TEST_CASE("Windows reports reference cycles")
{
    auto const source = makeDefaultCpuCounterSource();
    REQUIRE(source->supports(CpuCounter::ReferenceCycles));
    CHECK(!source->supports(CpuCounter::Instructions));
    CHECK(!source->supports(CpuCounter::Cycles));
}
