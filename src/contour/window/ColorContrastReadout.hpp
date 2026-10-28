// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The contrast readout beside a colour well on the Colors page: the WCAG ratio of a colour against what it
/// sits on, as "4.80 : 1, passes", and whether it meets the rule the page holds text to. Qt-free.

#include <vtbackend/core/Color.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace contour::window
{

/// The ratio text ink needs against its background (WCAG 2 level AA).
inline constexpr auto TextContrastRule = 4.5;

/// Whether a colour meets the contrast rule against what it sits on.
enum class ContrastVerdict : uint8_t
{
    Passes = 0, ///< The ratio is at least the rule.
    Fails,      ///< The ratio is below the rule.
};

/// What the readout says of a pair of colours.
struct ContrastReadout
{
    double ratio = 1.0;                                ///< The WCAG ratio, 1 to 21.
    ContrastVerdict verdict = ContrastVerdict::Passes; ///< Whether @ref ratio meets the rule.
};

/// The readout for @p foreground on @p background.
/// @param foreground The colour read.
/// @param background What it sits on.
/// @param required   The ratio it must reach.
/// @return The ratio and the verdict.
[[nodiscard]] inline ContrastReadout contrastReadoutOf(vtbackend::RGBColor foreground,
                                                       vtbackend::RGBColor background,
                                                       double required = TextContrastRule)
{
    auto const ratio = vtbackend::contrastRatio(foreground, background);
    return { .ratio = ratio,
             .verdict = ratio >= required ? ContrastVerdict::Passes : ContrastVerdict::Fails };
}

/// @param verdict The verdict.
/// @return The word the readout says it with, so that a colour alone never carries it.
[[nodiscard]] constexpr std::string_view contrastVerdictWord(ContrastVerdict verdict) noexcept
{
    return verdict == ContrastVerdict::Passes ? "passes" : "fails";
}

/// @param readout The readout.
/// @return The page's text: the ratio to two decimals and the verdict, as in "4.54 : 1, passes". The ratio
///         is truncated, never rounded, so that a failing 4.499 reads "4.49", not the "4.50" of the rule.
[[nodiscard]] inline std::string formatContrast(ContrastReadout readout)
{
    constexpr auto Hundredths = 100.0;
    // The ratio is a quotient of floating-point sums: 21 may come out a hair under, which must not
    // read 20.99.
    constexpr auto Slack = 1e-9;
    auto const truncated = std::floor((readout.ratio * Hundredths) + Slack) / Hundredths;
    return std::format("{:.2f} : 1, {}", truncated, contrastVerdictWord(readout.verdict));
}

} // namespace contour::window
