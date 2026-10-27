// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ScrollBarMode.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace contour::session
{

/// Whether a pane's scrollbar is drawn.
enum class ScrollbarVisibility : uint8_t
{
    Hidden = 0, //!< Not drawn.
    Shown,      //!< Drawn.
};

/// Whether the scrollbar may show over the alternate screen: `scrollbar.hide_in_alt_screen`, converted from
/// its YAML boolean at the boundary.
enum class AltScreenScrollbar : uint8_t
{
    Hidden = 0, //!< Hidden over the alternate screen (`hide_in_alt_screen: true`).
    Shown,      //!< Decided by the remaining rules, as over the primary screen.
};

/// Everything the scrollbar's visibility depends on. Every member has the value of a fresh primary screen
/// with no scrollback, so a case names only what it is about.
struct ScrollbarVisibilityInputs
{
    config::ScrollBarPosition position = config::ScrollBarPosition::Right; ///< `scrollbar.position`.
    config::ScrollBarMode mode = config::ScrollBarMode::Overlay;           ///< The resolved `scrollbar.mode`.
    vtbackend::ScreenType screen = vtbackend::ScreenType::Primary;         ///< The screen on display.
    AltScreenScrollbar altScreen = AltScreenScrollbar::Hidden; ///< `scrollbar.hide_in_alt_screen`.
    int64_t historyLines = 0;                                  ///< The rows there are to scroll back through.
    int64_t scrollOffset = 0; ///< How far the view is scrolled back; 0 is the bottom.
    vtbackend::MarksAbove marksAbove = vtbackend::MarksAbove::None; ///< Held marks above the view.
};

/// One rule of the scrollbar's visibility: when it applies, and what it decides.
struct ScrollbarVisibilityRule
{
    std::string_view name;                                      ///< What the rule says, for diagnostics.
    bool (*applies)(ScrollbarVisibilityInputs const&) noexcept; ///< Whether the rule decides these inputs.
    ScrollbarVisibility result;                                 ///< What it decides when it applies.
};

/// The rules, in order; the first that applies decides (spec §3.11). The last applies to everything, so
/// every input is decided.
inline constexpr auto ScrollbarVisibilityRules = std::array {
    ScrollbarVisibilityRule { .name = "a hidden position hides the bar",
                              .applies =
                                  [](ScrollbarVisibilityInputs const& in) noexcept {
                                      return in.position == config::ScrollBarPosition::Hidden;
                                  },
                              .result = ScrollbarVisibility::Hidden },
    ScrollbarVisibilityRule { .name = "the alternate screen hides it while hide_in_alt_screen is on",
                              .applies =
                                  [](ScrollbarVisibilityInputs const& in) noexcept {
                                      return in.screen == vtbackend::ScreenType::Alternate
                                             && in.altScreen == AltScreenScrollbar::Hidden;
                                  },
                              .result = ScrollbarVisibility::Hidden },
    ScrollbarVisibilityRule {
        .name = "no scrollback leaves nothing to show",
        .applies = [](ScrollbarVisibilityInputs const& in) noexcept { return in.historyLines <= 0; },
        .result = ScrollbarVisibility::Hidden },
    ScrollbarVisibilityRule { .name = "an Always bar shows",
                              .applies =
                                  [](ScrollbarVisibilityInputs const& in) noexcept {
                                      return in.mode == config::ScrollBarMode::Always;
                                  },
                              .result = ScrollbarVisibility::Shown },
    ScrollbarVisibilityRule {
        .name = "an Overlay bar shows while scrolled back or while a held mark is above",
        .applies =
            [](ScrollbarVisibilityInputs const& in) noexcept {
                return in.scrollOffset > 0 || in.marksAbove == vtbackend::MarksAbove::Some;
            },
        .result = ScrollbarVisibility::Shown },
    ScrollbarVisibilityRule { .name = "an Overlay bar hides at the bottom",
                              .applies = [](ScrollbarVisibilityInputs const&) noexcept { return true; },
                              .result = ScrollbarVisibility::Hidden },
};

/// The rule that decides @p in.
/// @param in What the bar's visibility depends on.
/// @return The first rule of ScrollbarVisibilityRules that applies.
[[nodiscard]] constexpr ScrollbarVisibilityRule const& decidingScrollbarRule(
    ScrollbarVisibilityInputs const& in) noexcept
{
    for (auto const& rule: ScrollbarVisibilityRules)
        if (rule.applies(in))
            return rule;
    return ScrollbarVisibilityRules.back();
}

/// Whether the scrollbar is drawn for @p in.
/// @param in What the bar's visibility depends on.
/// @return What the first applying rule of ScrollbarVisibilityRules decides.
[[nodiscard]] constexpr ScrollbarVisibility resolveScrollbarVisibility(
    ScrollbarVisibilityInputs const& in) noexcept
{
    return decidingScrollbarRule(in).result;
}

} // namespace contour::session
