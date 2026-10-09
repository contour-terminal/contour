// SPDX-License-Identifier: Apache-2.0
#include <contour/display/GpuSelection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace contour;
using namespace contour::display;

namespace
{
// Listed discrete-first on purpose: the ranking, not the listing order, must decide.
auto const Hybrid = std::vector<GpuCandidate> {
    { .title = "NVIDIA GeForce RTX 4070",
      .id = { 0x10de, 0x2820 },
      .kind = GpuKind::Discrete,
      .output = GpuOutput::Offscreen,
      .driver = "nouveau" },
    { .title = "llvmpipe",
      .id = { 0x0000, 0x0000 },
      .kind = GpuKind::Cpu,
      .output = GpuOutput::Offscreen,
      .driver = {} },
    { .title = "Intel integrated GPU",
      .id = { 0x8086, 0xa788 },
      .kind = GpuKind::Integrated,
      .output = GpuOutput::DrivesDisplay,
      .driver = "i915" },
};

[[nodiscard]] config::GpuSelector selector(config::GpuPreference preference)
{
    return config::GpuSelector { .preference = preference, .id = std::nullopt };
}
} // namespace

TEST_CASE("chooseGpu: each preference follows its ranking row", "[gpu]")
{
    CHECK(chooseGpu(Hybrid, selector(config::GpuPreference::Auto))->index == 2);
    CHECK(chooseGpu(Hybrid, selector(config::GpuPreference::Integrated))->index == 2);
    CHECK(chooseGpu(Hybrid, selector(config::GpuPreference::Discrete))->index == 0);
}

TEST_CASE("chooseGpu: integrated falls back to discrete, never to the CPU rasterizer", "[gpu]")
{
    auto const dgpuOnly = std::vector<GpuCandidate> { Hybrid[1], Hybrid[0] };
    CHECK(chooseGpu(dgpuOnly, selector(config::GpuPreference::Integrated))->index == 1);
}

TEST_CASE("chooseGpu: a specific id is honoured; a missing one falls back to auto and says so", "[gpu]")
{
    auto const hit = chooseGpu(
        Hybrid, { .preference = config::GpuPreference::Specific, .id = config::PciId { 0x10de, 0x2820 } });
    REQUIRE(hit.has_value());
    CHECK(hit->index == 0);
    CHECK(hit->outcome == RequestOutcome::Satisfied);

    auto const miss = chooseGpu(
        Hybrid, { .preference = config::GpuPreference::Specific, .id = config::PciId { 0x1002, 0x7340 } });
    REQUIRE(miss.has_value());
    CHECK(miss->index == 2);
    CHECK(miss->outcome == RequestOutcome::FellBack);
}

TEST_CASE("chooseGpu: no candidates means no choice", "[gpu]")
{
    CHECK_FALSE(chooseGpu({}, selector(config::GpuPreference::Auto)).has_value());
}
