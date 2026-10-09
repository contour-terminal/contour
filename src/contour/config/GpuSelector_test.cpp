// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GpuSelector.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string_view>

using namespace contour::config;
using namespace std::string_view_literals;

TEST_CASE("GpuSelector: keywords, ids, whitespace and case", "[config][gpu]")
{
    CHECK(parseGpuSelector("") == GpuSelector {});
    CHECK(parseGpuSelector("auto") == GpuSelector {});
    CHECK(parseGpuSelector("Integrated")->preference == GpuPreference::Integrated);
    CHECK(parseGpuSelector(" DISCRETE\t")->preference == GpuPreference::Discrete);

    auto const specific = parseGpuSelector("10DE:2820 ");
    REQUIRE(specific.has_value());
    CHECK(specific->preference == GpuPreference::Specific);
    CHECK(specific->id == PciId { .vendor = 0x10de, .device = 0x2820 });

    CHECK_FALSE(parseGpuSelector("rtx 4070").has_value());
    CHECK_FALSE(parseGpuSelector("10de:").has_value());
    CHECK_FALSE(parseGpuSelector("10de2:2820").has_value());
    CHECK_FALSE(parseGpuSelector("10de:28g0").has_value());
}

TEST_CASE("GpuSelector: formatting round-trips", "[config][gpu]")
{
    for (auto const text: { "auto"sv, "integrated"sv, "discrete"sv, "10de:2820"sv, "8086:a788"sv })
        CHECK(std::format("{}", parseGpuSelector(text).value()) == text);
    CHECK(std::format("{}", PciId { .vendor = 0x8086, .device = 0x00a7 }) == "8086:00a7");
}
