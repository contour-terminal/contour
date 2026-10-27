// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The pure decisions of secure input: whether a pane is locked, whether the OS's secure keyboard entry is
/// on, and where the lock is drawn. Qt-free.

#include <contour/config/SecureInputMode.hpp>
#include <contour/platform/SecureInput.hpp>

#include <vtpty/InputEcho.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace contour::window
{

/// Whether a pane shows the lock.
enum class SecureIndicator : uint8_t
{
    None = 0, ///< No lock.
    Locked,   ///< The program in the pane hides what is typed.
};

/// Whether a window of this application is the one the OS sends keys to.
enum class AppActivity : uint8_t
{
    Background = 0, ///< Another application, or none of our windows, has focus.
    Foreground,     ///< One of our windows has focus.
};

/// Whether a place the lock could be drawn is on screen; and, in a rule's answer, whether the lock goes
/// there.
enum class SpotShown : uint8_t
{
    No = 0,
    Yes,
};

/// The number of SpotShown enumerators, derived from the last one.
inline constexpr size_t SpotShownCount = static_cast<size_t>(SpotShown::Yes) + 1;

/// @param mode The configured `secure_input`.
/// @param echo What the pane's terminal does with typed text.
/// @return Locked only under `auto` while the terminal hides typing (a password prompt).
[[nodiscard]] constexpr SecureIndicator secureIndicatorFor(config::SecureInputMode mode,
                                                           vtpty::InputEcho echo) noexcept
{
    return mode == config::SecureInputMode::Auto && echo == vtpty::InputEcho::Hidden ? SecureIndicator::Locked
                                                                                     : SecureIndicator::None;
}

/// Whether the OS's secure keyboard entry should be on. It is process-wide, so it follows the one focused
/// pane: on while that pane is locked and the application is in front.
/// @param mode        The configured `secure_input`.
/// @param app         Whether one of our windows has focus.
/// @param focusedEcho The focused pane's echo; Unknown when no pane has focus.
/// @return On only for a locked focused pane in the front application.
[[nodiscard]] constexpr platform::SecureInputState desiredSecureInput(config::SecureInputMode mode,
                                                                      AppActivity app,
                                                                      vtpty::InputEcho focusedEcho) noexcept
{
    return app == AppActivity::Foreground && secureIndicatorFor(mode, focusedEcho) == SecureIndicator::Locked
               ? platform::SecureInputState::On
               : platform::SecureInputState::Off;
}

/// Where the lock goes, for one combination of what is on screen.
struct SecureIndicatorRule
{
    SpotShown header = SpotShown::No;       ///< Whether the pane's header is shown.
    SpotShown tabWell = SpotShown::No;      ///< Whether the tab well is shown.
    SpotShown band = SpotShown::No;         ///< Whether a title band is always on screen.
    SpotShown lockInHeader = SpotShown::No; ///< The lock goes in the pane header.
    SpotShown lockOnTab = SpotShown::No;    ///< The lock goes on the tab.
    SpotShown bandChip = SpotShown::No;     ///< The lock goes in a title-band chip.
    SpotShown lockInPane = SpotShown::No;   ///< The lock goes in an in-pane pill over the grid.
};

/// Every place the lock is drawn, one row per (header, tab well, band) combination: wherever the pane or its
/// tab is shown; else in the band's chip where a band is; else (one tab, one pane, no header, well or band --
/// the Flat rows under `tab_bar: multiple`) in a pill over the pane's blank cells. The band decides only the
/// last case, so the rows with a header or a well answer alike for either band.
inline constexpr auto SecureIndicatorRules = std::array {
    SecureIndicatorRule { .header = SpotShown::Yes,
                          .tabWell = SpotShown::Yes,
                          .band = SpotShown::Yes,
                          .lockInHeader = SpotShown::Yes,
                          .lockOnTab = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::Yes,
                          .tabWell = SpotShown::Yes,
                          .band = SpotShown::No,
                          .lockInHeader = SpotShown::Yes,
                          .lockOnTab = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::Yes,
                          .tabWell = SpotShown::No,
                          .band = SpotShown::Yes,
                          .lockInHeader = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::Yes,
                          .tabWell = SpotShown::No,
                          .band = SpotShown::No,
                          .lockInHeader = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::No,
                          .tabWell = SpotShown::Yes,
                          .band = SpotShown::Yes,
                          .lockOnTab = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::No,
                          .tabWell = SpotShown::Yes,
                          .band = SpotShown::No,
                          .lockOnTab = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::No,
                          .tabWell = SpotShown::No,
                          .band = SpotShown::Yes,
                          .bandChip = SpotShown::Yes },
    SecureIndicatorRule { .header = SpotShown::No,
                          .tabWell = SpotShown::No,
                          .band = SpotShown::No,
                          .lockInPane = SpotShown::Yes },
};

/// @param header  Whether the pane's header is shown.
/// @param tabWell Whether the tab well is shown.
/// @param band    Whether a title band is always on screen (not one revealed on demand).
/// @return The rule of SecureIndicatorRules for that combination.
[[nodiscard]] constexpr SecureIndicatorRule const& secureIndicatorSpots(SpotShown header,
                                                                        SpotShown tabWell,
                                                                        SpotShown band) noexcept
{
    for (auto const& rule: SecureIndicatorRules)
        if (rule.header == header && rule.tabWell == tabWell && rule.band == band)
            return rule;
    return SecureIndicatorRules.back(); // unreachable: the table holds every combination
}

/// The words beside the lock and its accessible name.
/// @param support Whether the OS turns secure keyboard entry on here.
/// @return "Secure input on" where it does, "Input hidden" where only the terminal hides the text.
[[nodiscard]] constexpr std::string_view secureInputStateText(platform::SecureInputSupport support) noexcept
{
    switch (support)
    {
        case platform::SecureInputSupport::Native: return "Secure input on";
        case platform::SecureInputSupport::None: break;
    }
    return "Input hidden";
}

} // namespace contour::window
