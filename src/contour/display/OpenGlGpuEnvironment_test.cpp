// SPDX-License-Identifier: Apache-2.0
#include <contour/display/OpenGlGpuEnvironment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("OpenGL GPU environment: the marker value round-trips", "[gpu]")
{
    auto const assignments =
        std::vector<EnvironmentAssignment> { { .name = "__NV_PRIME_RENDER_OFFLOAD", .value = "1" },
                                             { .name = "__GLX_VENDOR_LIBRARY_NAME", .value = "nvidia" } };
    auto const value = selfSetMarkerValue(assignments);
    CHECK(value == "__NV_PRIME_RENDER_OFFLOAD,__GLX_VENDOR_LIBRARY_NAME");
    CHECK(selfSetNamesFrom(value)
          == std::vector<std::string> { "__NV_PRIME_RENDER_OFFLOAD", "__GLX_VENDOR_LIBRARY_NAME" });
}

TEST_CASE("OpenGL GPU environment: no assignments give an empty marker", "[gpu]")
{
    CHECK(selfSetMarkerValue({}).empty());
    CHECK(selfSetNamesFrom("").empty());
}

TEST_CASE("OpenGL GPU environment: stray commas and blanks in the marker are ignored", "[gpu]")
{
    CHECK(selfSetNamesFrom(",, DRI_PRIME ,,\t,X,") == std::vector<std::string> { "DRI_PRIME", "X" });
    CHECK(selfSetNamesFrom(" , ").empty());
}

TEST_CASE("OpenGL GPU fallback: the relaunched Contour gets the user's environment back", "[gpu]")
{
    auto const inherited = std::vector<std::string_view> {
        "PATH=/usr/bin", "DRI_PRIME=10de:2820", "CONTOUR_SELF_SET_ENVIRONMENT=DRI_PRIME", "HOME=/home/u"
    };
    auto const selfSet = std::vector<std::string> { "DRI_PRIME", "CONTOUR_SELF_SET_ENVIRONMENT" };
    auto entries = gpuFallbackRelaunchEnvironment(inherited, selfSet, "NVIDIA GeForce RTX 4070");
    std::ranges::sort(entries);
    CHECK(entries
          == std::vector<std::string> {
              "CONTOUR_GPU_FALLBACK=NVIDIA GeForce RTX 4070", "HOME=/home/u", "PATH=/usr/bin" });
}

TEST_CASE("OpenGL GPU fallback: the GPU now in use is the one driving the display", "[gpu]")
{
    auto const gpus = std::vector<GpuCandidate> {
        gpu({ 0x10de, 0x2820 }, GpuOutput::Offscreen, "nouveau"),
        GpuCandidate { .title = "Intel integrated GPU",
                       .id = { 0x8086, 0xa788 },
                       .kind = GpuKind::Integrated,
                       .output = GpuOutput::DrivesDisplay,
                       .driver = "i915" },
    };
    CHECK(displayGpuTitle(gpus) == "Intel integrated GPU");
    CHECK(displayGpuTitle({}) == "the system's default GPU");
}
