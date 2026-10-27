// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsControls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string_view>

using contour::window::controlAccepts;
using contour::window::defaultControlFor;
using contour::window::formatNumber;
using contour::window::NumberRange;
using contour::window::NumberUnit;
using contour::window::SettingsControl;
using contour::window::SettingsControlCount;
using contour::window::SettingsControlRules;
using contour::window::SettingsValueType;
using contour::window::SettingsValueTypeCount;
using namespace std::string_view_literals;

namespace
{
constexpr auto Percent =
    NumberRange { .min = 0.0, .max = 1.0, .step = 0.01, .decimals = 0, .unit = NumberUnit::Percent };
} // namespace

TEST_CASE("SettingsControls: each value type picks its control", "[settingscontrols]")
{
    CHECK(defaultControlFor(SettingsValueType::Bool, {}, {}) == SettingsControl::Switch);
    CHECK(defaultControlFor(SettingsValueType::Int, {}, {}) == SettingsControl::Stepper);
    CHECK(defaultControlFor(SettingsValueType::String, {}, {}) == SettingsControl::TextField);
    CHECK(defaultControlFor(SettingsValueType::FontFamily, {}, {}) == SettingsControl::FontPicker);
    CHECK(defaultControlFor(SettingsValueType::Color, {}, {}) == SettingsControl::ColorWell);
}

TEST_CASE("SettingsControls: an enum is segmented while its labels fit, a pop-up otherwise",
          "[settingscontrols]")
{
    auto const four = std::array { "Off"sv, "Left"sv, "Right"sv, "Both"sv };
    CHECK(defaultControlFor(SettingsValueType::Enum, four, {}) == SettingsControl::Segmented);

    auto const five = std::array { "Off"sv, "Left"sv, "Right"sv, "Both"sv, "Either"sv };
    CHECK(defaultControlFor(SettingsValueType::Enum, five, {}) == SettingsControl::PopUp);

    auto const long13 = std::array { "Off"sv, "Thirteen char"sv };
    REQUIRE(long13[1].size() == 13);
    CHECK(defaultControlFor(SettingsValueType::Enum, long13, {}) == SettingsControl::PopUp);

    // Characters, not bytes: twelve of them fit even when one takes two bytes.
    auto const twelveAccented = std::array { "Off"sv, "Accénts fit!"sv };
    REQUIRE(twelveAccented[1].size() == 13);
    CHECK(defaultControlFor(SettingsValueType::Enum, twelveAccented, {}) == SettingsControl::Segmented);

    CHECK(defaultControlFor(SettingsValueType::Enum, {}, {}) == SettingsControl::PopUp);
}

TEST_CASE("SettingsControls: a fraction is a slider, any other number a stepper", "[settingscontrols]")
{
    CHECK(defaultControlFor(SettingsValueType::Double, {}, NumberRange { .min = 0, .max = 1, .step = 0.05 })
          == SettingsControl::Slider);
    CHECK(defaultControlFor(SettingsValueType::Double, {}, NumberRange { .min = 6, .max = 72, .step = 0.5 })
          == SettingsControl::Stepper);
    CHECK(defaultControlFor(SettingsValueType::Double, {}, std::nullopt) == SettingsControl::Stepper);
    CHECK(defaultControlFor(SettingsValueType::Int, {}, NumberRange { .min = 0, .max = 1 })
          == SettingsControl::Stepper);
}

TEST_CASE("SettingsControls: every default control is one its type accepts", "[settingscontrols]")
{
    REQUIRE(SettingsControlRules.size() == SettingsControlCount);
    auto const labels = std::array { "One"sv, "Two"sv };
    for (auto const index: std::views::iota(std::size_t { 0 }, SettingsValueTypeCount))
    {
        auto const type = static_cast<SettingsValueType>(index);
        INFO(index);
        CHECK(controlAccepts(defaultControlFor(type, labels, Percent), type));
        CHECK(controlAccepts(defaultControlFor(type, {}, std::nullopt), type));
    }
    CHECK_FALSE(controlAccepts(SettingsControl::Switch, SettingsValueType::Int));
    CHECK_FALSE(controlAccepts(SettingsControl::Slider, SettingsValueType::Int));
    CHECK_FALSE(controlAccepts(SettingsControl::FontPicker, SettingsValueType::String));
    CHECK(controlAccepts(SettingsControl::TextField, SettingsValueType::Double));
}

TEST_CASE("SettingsControls: a number reads in its unit", "[settingscontrols]")
{
    CHECK(formatNumber(0.15, Percent) == "15 %");
    CHECK(formatNumber(1.0, Percent) == "100 %");
    auto const lines =
        NumberRange { .min = -1, .max = 1e9, .step = 1, .decimals = 0, .unit = NumberUnit::Lines };
    CHECK(formatNumber(10000, lines) == "10 000");
    CHECK(formatNumber(1234567, lines) == "1 234 567");
    CHECK(formatNumber(999, lines) == "999");
    CHECK(formatNumber(-1, lines) == "-1");
    CHECK(formatNumber(12.5, NumberRange { .step = 0.5, .decimals = 1, .unit = NumberUnit::Points })
          == "12.5 pt");
    CHECK(formatNumber(250, NumberRange { .unit = NumberUnit::Milliseconds }) == "250 ms");
    CHECK(formatNumber(3.14159, NumberRange { .decimals = 2 }) == "3.14");
}
