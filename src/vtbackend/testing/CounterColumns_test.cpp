// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/CounterColumns.hpp>

#include <core/platform/testing/FakeCpuCounterSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using core::platform::CpuCounter;
using core::platform::CpuCounterSample;
using core::platform::testing::FakeCpuCounterSource;
using vtbackend::testing::formatCounterAvailability;
using vtbackend::testing::formatCounterColumns;

namespace
{
[[nodiscard]] CpuCounterSample sampleOf(std::optional<std::uint64_t> instructions,
                                        std::optional<std::uint64_t> cycles)
{
    auto sample = CpuCounterSample {};
    if (instructions)
        sample.set(CpuCounter::Instructions, *instructions);
    if (cycles)
        sample.set(CpuCounter::Cycles, *cycles);
    return sample;
}

[[nodiscard]] FakeCpuCounterSource instructionsAndCycles()
{
    return FakeCpuCounterSource { {}, { CpuCounter::Instructions, CpuCounter::Cycles } };
}
} // namespace

TEST_CASE("CounterColumns.scales_to_an_SI_prefix", "[bench]")
{
    auto const source = instructionsAndCycles();
    auto const delta = sampleOf(1'230'000'000, 450'000'000);
    CHECK(formatCounterColumns(delta, source, 1) == "    1.23 Ginstr  450.00 Mcyc");
    CHECK(formatCounterColumns(delta, source, 1000) == "    1.23 Minstr  450.00 kcyc");
}

TEST_CASE("CounterColumns.absent_measure_prints_a_dash", "[bench]")
{
    auto const source = instructionsAndCycles();
    CHECK(formatCounterColumns(sampleOf(std::nullopt, 450'000'000), source, 1)
          == "       — instr  450.00 Mcyc");
}

TEST_CASE("CounterColumns.unsupported_source_prints_nothing", "[bench]")
{
    auto const source = FakeCpuCounterSource { {}, {} };
    CHECK(formatCounterColumns(sampleOf(1, 2), source, 1).empty());
    CHECK(formatCounterAvailability(source) == "CPU counters unavailable: fake: unsupported");
}

TEST_CASE("CounterColumns.supported_source_is_available", "[bench]")
{
    auto const source = FakeCpuCounterSource { {}, { CpuCounter::Cycles } };
    CHECK(formatCounterAvailability(source).empty());
}

TEST_CASE("CounterColumns.prefix_is_picked_after_rounding", "[bench]")
{
    auto const source = FakeCpuCounterSource { {}, { CpuCounter::Instructions } };
    auto const columns = [&](std::uint64_t value) {
        return formatCounterColumns(sampleOf(value, std::nullopt), source, 1);
    };
    CHECK(columns(0) == "    0.00  instr");
    CHECK(columns(999) == "  999.00  instr");
    CHECK(columns(1000) == "    1.00 kinstr");
    CHECK(columns(999'995) == "    1.00 Minstr");
    CHECK(columns(1'000'000'000'000) == "    1.00 Tinstr");
}

TEST_CASE("CounterColumns.reference_cycles_have_their_own_unit", "[bench]")
{
    auto const source = FakeCpuCounterSource { {}, { CpuCounter::ReferenceCycles } };
    auto sample = CpuCounterSample {};
    sample.set(CpuCounter::ReferenceCycles, 2'500'000);
    CHECK(formatCounterColumns(sample, source, 1) == "    2.50 Mrefcyc");
}
