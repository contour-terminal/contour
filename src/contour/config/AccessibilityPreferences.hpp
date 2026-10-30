// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::config
{

/// Whether the chrome animates fully or swaps state with a short cross-fade.
enum class MotionPreference : uint8_t
{
    Full = 0, //!< Chrome animations run as designed.
    Reduced,  //!< Durations collapse to a cross-fade and travel distances to zero.
};

/// Whether the chrome may use translucent materials.
enum class TransparencyPreference : uint8_t
{
    Allowed = 0, //!< Glass and mica materials are used where the style asks for them.
    Reduced,     //!< Translucent materials fall back to a solid fill.
};

/// How much separation the chrome draws between surfaces.
enum class ContrastPreference : uint8_t
{
    Standard = 0, //!< The style's own separators and rings.
    Increased,    //!< Stronger separators, a heavier active-card ring and a dimmed inactive pane.
};

/// The user's operating-system accessibility choices, as the chrome token resolver consumes them.
///
/// Qt-free on purpose: an adapter reads the platform setting and fills this in, and the resolver
/// stays a pure function of it.
struct AccessibilityPreferences
{
    MotionPreference motion = MotionPreference::Full;                      ///< Animation preference.
    TransparencyPreference transparency = TransparencyPreference::Allowed; ///< Material preference.
    ContrastPreference contrast = ContrastPreference::Standard;            ///< Contrast preference.

    [[nodiscard]] bool operator==(AccessibilityPreferences const&) const = default;
};

} // namespace contour::config
