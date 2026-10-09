// SPDX-License-Identifier: Apache-2.0
#include <contour/display/OpenGlGpuEnvironment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace contour;
using namespace contour::display;

namespace
{
[[nodiscard]] bool nothingSet(std::string_view /*name*/)
{
    return false;
}

[[nodiscard]] GpuCandidate gpu(config::PciId id, GpuOutput output, std::string driver)
{
    return GpuCandidate {
        .title = "x", .id = id, .kind = GpuKind::Discrete, .output = output, .driver = std::move(driver)
    };
}
} // namespace

TEST_CASE("OpenGL GPU environment: the display GPU needs nothing", "[gpu]")
{
    CHECK(openGlSelectionEnvironment(gpu({ 0x8086, 0xa788 }, GpuOutput::DrivesDisplay, "i915"), nothingSet)
              .empty());
}

TEST_CASE("OpenGL GPU environment: Mesa drivers get DRI_PRIME=vendor:device", "[gpu]")
{
    CHECK(openGlSelectionEnvironment(gpu({ 0x10de, 0x2820 }, GpuOutput::Offscreen, "nouveau"), nothingSet)
          == std::vector<EnvironmentAssignment> { { .name = "DRI_PRIME", .value = "10de:2820" } });
}

TEST_CASE("OpenGL GPU environment: the NVIDIA driver gets PRIME render offload", "[gpu]")
{
    CHECK(
        openGlSelectionEnvironment(gpu({ 0x10de, 0x2820 }, GpuOutput::Offscreen, "nvidia"), nothingSet)
        == std::vector<EnvironmentAssignment> { { .name = "__NV_PRIME_RENDER_OFFLOAD", .value = "1" },
                                                { .name = "__GLX_VENDOR_LIBRARY_NAME", .value = "nvidia" } });
}

TEST_CASE("OpenGL GPU environment: a variable the user already set wins", "[gpu]")
{
    auto const userSetDriPrime = [](std::string_view name) {
        return name == "DRI_PRIME";
    };
    CHECK(
        openGlSelectionEnvironment(gpu({ 0x10de, 0x2820 }, GpuOutput::Offscreen, "nouveau"), userSetDriPrime)
            .empty());
}
