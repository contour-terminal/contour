// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which control a Settings row is edited with, as data: a value type and its range or option labels pick
/// the control (R-i), and one table says which controls can edit which types. Qt-free.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace contour::window
{

/// What a Settings row's value is.
enum class SettingsValueType : uint8_t
{
    Bool = 0,
    Int,
    Double,
    String,
    Enum,
    FontFamily, ///< A font family name, picked from the installed monospace families.
    Color,      ///< A colour, "#rrggbb"; empty where the setting may have none.
};

/// How many value types there are.
inline constexpr std::size_t SettingsValueTypeCount = static_cast<std::size_t>(SettingsValueType::Color) + 1;

/// The token a row's `type` carries for each value type, in enumerator order. The first five are the
/// spellings the page has always switched on.
inline constexpr auto SettingsValueTypeTokens = std::array<std::string_view, SettingsValueTypeCount> {
    "bool", "int", "double", "string", "enum", "fontFamily", "color",
};

/// The token a row's `type` carries for @p type.
/// @param type The value type.
/// @return Its token.
[[nodiscard]] constexpr std::string_view settingsValueTypeToken(SettingsValueType type) noexcept
{
    return SettingsValueTypeTokens[static_cast<std::size_t>(type)];
}

/// The value type @p token names.
/// @param token A row's `type`.
/// @return The type, or nullopt when no type carries that token.
[[nodiscard]] constexpr std::optional<SettingsValueType> settingsValueTypeFromToken(
    std::string_view token) noexcept
{
    auto const found = std::ranges::find(SettingsValueTypeTokens, token);
    if (found == SettingsValueTypeTokens.end())
        return std::nullopt;
    return static_cast<SettingsValueType>(std::distance(SettingsValueTypeTokens.begin(), found));
}

/// The control a Settings row is edited with.
enum class SettingsControl : uint8_t
{
    Switch = 0,
    Segmented,
    PopUp,
    Stepper,
    Slider,
    TextField,
    FontPicker,
    ColorWell,
};

/// How many controls there are.
inline constexpr std::size_t SettingsControlCount = static_cast<std::size_t>(SettingsControl::ColorWell) + 1;

/// The unit a number is shown in.
enum class NumberUnit : uint8_t
{
    None = 0,
    Percent,      ///< A fraction of one, shown as a whole percentage: 0.15 reads "15 %".
    Points,       ///< " pt".
    Milliseconds, ///< " ms".
    Lines,        ///< A count of lines, its thousands grouped with a thin space: "10 000".
};

/// The values a number may take, and how it is shown.
struct NumberRange
{
    double min = 0.0;                   ///< The smallest value; -infinity where there is no bound.
    double max = 0.0;                   ///< The largest value; +infinity where there is no bound.
    double step = 1.0;                  ///< One step of a stepper, a slider or an arrow key.
    int decimals = 0;                   ///< Digits after the point.
    NumberUnit unit = NumberUnit::None; ///< What the number counts.

    [[nodiscard]] bool operator==(NumberRange const&) const = default;
};

/// The most options a segmented control shows; more make a pop-up.
inline constexpr std::size_t SegmentedMaxOptions = 4;

/// The longest option label, in characters, a segmented control shows; a longer one makes a pop-up.
inline constexpr std::size_t SegmentedMaxLabel = 12;

/// The widest range a slider spans: a fraction of one. A wider one is a stepper.
inline constexpr double SliderMaxSpan = 1.0;

namespace detail
{
    /// The characters @p text holds: its UTF-8 bytes that do not continue a character.
    [[nodiscard]] constexpr std::size_t characterCount(std::string_view text) noexcept
    {
        constexpr auto ContinuationMask = 0b1100'0000U;
        constexpr auto ContinuationBits = 0b1000'0000U;
        return static_cast<std::size_t>(std::ranges::count_if(text, [](char ch) {
            return (static_cast<unsigned char>(ch) & ContinuationMask) != ContinuationBits;
        }));
    }
} // namespace detail

/// The control R-i gives a row of @p type.
/// @param type The row's value type.
/// @param optionLabels An enum's option labels, as shown; empty for any other type.
/// @param range A number's range, if it has one.
/// @return Switch for a bool; Segmented for an enum of at most SegmentedMaxOptions labels of at most
///         SegmentedMaxLabel characters each, else PopUp; Slider for a double whose range spans at most
///         SliderMaxSpan, else Stepper, as for an int; TextField for a string; FontPicker; ColorWell.
[[nodiscard]] constexpr SettingsControl defaultControlFor(SettingsValueType type,
                                                          std::span<std::string_view const> optionLabels,
                                                          std::optional<NumberRange> range) noexcept
{
    switch (type)
    {
        case SettingsValueType::Bool: return SettingsControl::Switch;
        case SettingsValueType::Enum:
            if (!optionLabels.empty() && optionLabels.size() <= SegmentedMaxOptions
                && std::ranges::all_of(optionLabels, [](std::string_view label) {
                       return detail::characterCount(label) <= SegmentedMaxLabel;
                   }))
                return SettingsControl::Segmented;
            return SettingsControl::PopUp;
        case SettingsValueType::Double:
            if (range && range->max - range->min <= SliderMaxSpan)
                return SettingsControl::Slider;
            return SettingsControl::Stepper;
        case SettingsValueType::Int: return SettingsControl::Stepper;
        case SettingsValueType::String: return SettingsControl::TextField;
        case SettingsValueType::FontFamily: return SettingsControl::FontPicker;
        case SettingsValueType::Color: return SettingsControl::ColorWell;
    }
    return SettingsControl::TextField;
}

/// The value types one control edits.
struct SettingsControlRule
{
    SettingsControl control = SettingsControl::Switch; ///< The control.
    std::uint8_t accepts = 0;                          ///< One bit per SettingsValueType it edits.
};

/// The bit @p type takes in SettingsControlRule::accepts.
[[nodiscard]] constexpr std::uint8_t valueTypeBit(SettingsValueType type) noexcept
{
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(type));
}

/// Which types each control edits, one row per control in enumerator order.
inline constexpr auto SettingsControlRules = std::array {
    SettingsControlRule { .control = SettingsControl::Switch,
                          .accepts = valueTypeBit(SettingsValueType::Bool) },
    SettingsControlRule { .control = SettingsControl::Segmented,
                          .accepts = valueTypeBit(SettingsValueType::Enum) },
    SettingsControlRule { .control = SettingsControl::PopUp,
                          .accepts = valueTypeBit(SettingsValueType::Enum) },
    SettingsControlRule { .control = SettingsControl::Stepper,
                          .accepts = static_cast<std::uint8_t>(valueTypeBit(SettingsValueType::Int)
                                                               | valueTypeBit(SettingsValueType::Double)) },
    SettingsControlRule { .control = SettingsControl::Slider,
                          .accepts = valueTypeBit(SettingsValueType::Double) },
    SettingsControlRule { .control = SettingsControl::TextField,
                          .accepts = static_cast<std::uint8_t>(valueTypeBit(SettingsValueType::String)
                                                               | valueTypeBit(SettingsValueType::Int)
                                                               | valueTypeBit(SettingsValueType::Double)) },
    SettingsControlRule { .control = SettingsControl::FontPicker,
                          .accepts = valueTypeBit(SettingsValueType::FontFamily) },
    SettingsControlRule { .control = SettingsControl::ColorWell,
                          .accepts = valueTypeBit(SettingsValueType::Color) },
};

static_assert(SettingsControlRules.size() == SettingsControlCount, "one rule per SettingsControl");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SettingsControlCount),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(SettingsControlRules[i].control) == i;
                                  }),
              "SettingsControlRules lists the controls in enumerator order");

/// Whether @p control can edit a value of @p type.
[[nodiscard]] constexpr bool controlAccepts(SettingsControl control, SettingsValueType type) noexcept
{
    return (SettingsControlRules[static_cast<std::size_t>(control)].accepts & valueTypeBit(type)) != 0;
}

namespace detail
{
    /// @p digits with a thin space (U+2009) between each group of three, counted from the right.
    [[nodiscard]] inline std::string groupThousands(std::string_view digits)
    {
        constexpr auto ThinSpace = std::string_view { " " };
        constexpr auto GroupSize = std::size_t { 3 };
        auto grouped = std::string {};
        for (auto const index: std::views::iota(std::size_t { 0 }, digits.size()))
        {
            if (index != 0 && (digits.size() - index) % GroupSize == 0)
                grouped += ThinSpace;
            grouped += digits[index];
        }
        return grouped;
    }
} // namespace detail

/// @p value as a row shows it, in the unit of @p range.
/// @param value The number.
/// @param range Its range; only its unit and decimals matter here.
/// @return "15 %" for 0.15 in Percent; "10 000" for 10000 Lines; "12.5 pt"; "250 ms"; else the value with
///         @p range's decimals.
[[nodiscard]] inline std::string formatNumber(double value, NumberRange const& range)
{
    constexpr auto PercentPerUnit = 100.0;
    switch (range.unit)
    {
        case NumberUnit::Percent: return std::format("{} %", std::llround(value * PercentPerUnit));
        case NumberUnit::Lines: {
            auto const lines = std::llround(value);
            auto const digits = std::to_string(lines < 0 ? -lines : lines);
            return std::format("{}{}", lines < 0 ? "-" : "", detail::groupThousands(digits));
        }
        case NumberUnit::Points: return std::format("{:.{}f} pt", value, range.decimals);
        case NumberUnit::Milliseconds: return std::format("{:.{}f} ms", value, range.decimals);
        case NumberUnit::None: break;
    }
    return std::format("{:.{}f}", value, range.decimals);
}

} // namespace contour::window
