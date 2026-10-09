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

TEST_CASE("inventoryKindOf: an adapter of unknown kind takes the inventory's kind for its id", "[gpu]")
{
    // Qt's Direct3D adapters report every hardware adapter as UnknownDevice.
    auto const unknown = [](config::PciId id) {
        return GpuCandidate {
            .title = "x", .id = id, .kind = GpuKind::Other, .output = GpuOutput::Offscreen, .driver = {}
        };
    };
    CHECK(inventoryKindOf(unknown({ 0x8086, 0xa788 }), Hybrid) == GpuKind::Integrated);
    CHECK(inventoryKindOf(unknown({ 0x10de, 0x2820 }), Hybrid) == GpuKind::Discrete);
    CHECK(inventoryKindOf(unknown({ 0x1002, 0x7340 }), Hybrid) == GpuKind::Other); // not in the inventory

    // A kind the adapter does report is kept: the API knows better than a heuristic.
    auto warp = unknown({ 0x1414, 0x008c });
    warp.kind = GpuKind::Cpu;
    CHECK(inventoryKindOf(warp, Hybrid) == GpuKind::Cpu);
}

TEST_CASE("gpuInUseLine: names the GPU the scene graph runs on", "[gpu]")
{
    auto const discrete = selector(config::GpuPreference::Discrete);
    CHECK(gpuInUseLine("NVIDIA GeForce RTX 4070 Laptop GPU", { 0x10de, 0x2820 }, "Vulkan", discrete)
          == "GPU: 'NVIDIA GeForce RTX 4070 Laptop GPU' 10de:2820 via Vulkan (requested: discrete)");
    // OpenGL reports no PCI id; a 0000:0000 would only mislead.
    CHECK(gpuInUseLine(
              "Intel Mesa Intel(R) Graphics (RPL-S)", {}, "OpenGL", selector(config::GpuPreference::Auto))
          == "GPU: 'Intel Mesa Intel(R) Graphics (RPL-S)' via OpenGL (requested: auto)");
}
