// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The contrast readout beside a colour well on the Colors page: the WCAG ratio of a colour against what it
/// sits on, as "4.8 : 1", and whether it meets the rule the page holds text to. Qt-free.

#include <vtbackend/core/Color.hpp>

#include <cstdint>
#include <format>
#include <string>

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

/// @param readout The readout.
/// @return Its ratio to one decimal, as the page shows it: "4.8 : 1".
[[nodiscard]] inline std::string formatContrast(ContrastReadout readout)
{
    return std::format("{:.1f} : 1", readout.ratio);
}

} // namespace contour::window
