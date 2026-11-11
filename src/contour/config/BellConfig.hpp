// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace contour::config
{

/// A profile's `bell`: what a bell sounds like and whether it alerts the window. Light, so that what rings a
/// bell outside a session can be handed it without the whole configuration.
struct Bell
{
    std::string sound = "default"; ///< `default`, `off`, or the path of a sound file.
    /// Whether a bell alerts the window too (taskbar, Dock). A config field parsed from YAML: the documented
    /// exception to enum over bool.
    bool alert = true;
    float volume = 1.0f; ///< The volume, 0 to 1.
};

} // namespace contour::config
