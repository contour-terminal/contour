// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ScrollBarMode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <utility>

using contour::config::configEnumFromToken;
using contour::config::configEnumValues;
using contour::config::resolveScrollBarMode;
using contour::config::ScrollBarMode;
using contour::config::ScrollBarModePreference;

namespace
{

/// Every answer a system can give: none, and each mode.
constexpr auto SystemAnswers = std::array<std::optional<ScrollBarMode>, 3> {
    std::nullopt,
    ScrollBarMode::Overlay,
    ScrollBarMode::Always,
};

} // namespace

TEST_CASE("scrollBarMode.resolves", "[scrollbarmode]")
{
    SECTION("System follows the system, and is Overlay where the system says nothing")
    {
        CHECK(resolveScrollBarMode(ScrollBarModePreference::System, std::nullopt) == ScrollBarMode::Overlay);
        CHECK(resolveScrollBarMode(ScrollBarModePreference::System, ScrollBarMode::Always)
              == ScrollBarMode::Always);
        CHECK(resolveScrollBarMode(ScrollBarModePreference::System, ScrollBarMode::Overlay)
              == ScrollBarMode::Overlay);
    }

    SECTION("Overlay and Always ignore every system value")
    {
        auto const named = std::array {
            std::pair { ScrollBarModePreference::Overlay, ScrollBarMode::Overlay },
            std::pair { ScrollBarModePreference::Always, ScrollBarMode::Always },
        };
        for (auto const& [preference, mode]: named)
            for (auto const system: SystemAnswers)
            {
                INFO("preference " << static_cast<int>(preference) << ", system "
                                   << (system ? static_cast<int>(*system) : -1));
                CHECK(resolveScrollBarMode(preference, system) == mode);
            }
    }

    SECTION("only System ever depends on the system")
    {
        for (auto const& info: configEnumValues<ScrollBarModePreference>())
        {
            auto const first = resolveScrollBarMode(info.value, SystemAnswers.front());
            auto const dependsOnSystem = std::ranges::any_of(SystemAnswers, [&](auto system) {
                return resolveScrollBarMode(info.value, system) != first;
            });
            INFO("preference " << info.token);
            CHECK(dependsOnSystem == (info.value == ScrollBarModePreference::System));
        }
    }
}

TEST_CASE("scrollBarMode.table", "[scrollbarmode]")
{
    auto const table = configEnumValues<ScrollBarModePreference>();
    // One row per enumerator, in enumerator order: the last enumerator closes the count.
    REQUIRE(table.size() == static_cast<size_t>(ScrollBarModePreference::Always) + 1);
    for (auto const i: std::views::iota(size_t { 0 }, table.size()))
        CHECK(static_cast<size_t>(table[i].value) == i);

    CHECK(configEnumFromToken<ScrollBarModePreference>("system") == ScrollBarModePreference::System);
    CHECK(configEnumFromToken<ScrollBarModePreference>("overlay") == ScrollBarModePreference::Overlay);
    CHECK(configEnumFromToken<ScrollBarModePreference>("ALWAYS") == ScrollBarModePreference::Always);
    CHECK_FALSE(configEnumFromToken<ScrollBarModePreference>("sideways").has_value());
}
