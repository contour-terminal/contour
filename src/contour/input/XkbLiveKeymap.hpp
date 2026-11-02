// SPDX-License-Identifier: Apache-2.0
//
// The libxkbcommon-backed XkbKeymap. Built on Linux only (see CMakeLists.txt): nothing else has XKB.
#pragma once

#include <contour/input/XkbKeymap.hpp>

#include <memory>
#include <string>

namespace contour::input
{

/// A keymap named by its RMLVO components: rules, model, layout, variant and options -- the form
/// `setxkbmap` takes. An empty component asks for libxkbcommon's default.
struct XkbNames
{
    std::string rules {};
    std::string model {};
    std::string layout {};
    std::string variant {};
    std::string options {};
};

/// Compiles a keymap from its names.
///
/// It is the part of the live adapter that needs no display, which is what lets the adapter be tested
/// without one. The first layout is the active group: nothing here learns of a group switch.
/// @param names The components to compile.
/// @return The keymap; nullptr when libxkbcommon cannot compile one (no XKB data installed).
[[nodiscard]] std::unique_ptr<XkbKeymap> makeXkbKeymapFromNames(XkbNames const& names);

} // namespace contour::input
