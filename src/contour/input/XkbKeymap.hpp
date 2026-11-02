// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace contour::input
{

/// What the X11 or Wayland keymap in force says about a key.
///
/// It is an interface because the keymap is an ambient operating-system resource -- the layouts the user
/// has configured, which can change while Contour runs -- and the tests substitute a fixed one rather
/// than inherit the developer's.
class XkbKeymap
{
  public:
    virtual ~XkbKeymap() = default;

    /// @param keycode The XKB keycode of the key (QKeyEvent::nativeScanCode()).
    /// @param group The XKB group (layout index) in force when the key was pressed, as the event reports it;
    ///        one beyond the last group the key has selects the last.
    /// @return The keysym the key produces at shift level 0 of @p group -- what it types with no modifier
    ///         held -- or 0 when the keymap has none for it.
    [[nodiscard]] virtual uint32_t levelZeroKeysymOf(uint32_t keycode, uint32_t group) const noexcept = 0;
};

/// One key of a fixed keymap.
struct FixedXkbKey
{
    uint32_t keycode; ///< The XKB keycode.
    uint32_t group;   ///< The group (layout index) the row is for.
    uint32_t keysym;  ///< What the key types at level 0 of that group.
};

/// @param keys The keymap, as one row per key and group.
/// @return A keymap that answers from the rows, copied; a group a key has no row for answers as its last
///         lower group does, and a keycode no row lists answers 0.
[[nodiscard]] std::unique_ptr<XkbKeymap> makeFixedXkbKeymap(std::span<FixedXkbKey const> keys);

/// The keymap the X server in force serves, where Contour runs on X11 (Qt's xcb platform).
///
/// Only the Linux build defines it. The keymap is fetched once, when called, and answers for the group each
/// key event reports, so a switch between the user's layouts is followed without asking the server again. A
/// rewrite of the keymap itself on the server is not followed: see docs/internals/keyboard-input.md.
/// @return The keymap; nullptr when none is reachable: no Qt application yet, a Qt platform that is not xcb
///         (Wayland and offscreen among them), or an X server without the XKB extension.
[[nodiscard]] std::unique_ptr<XkbKeymap> makeLiveXkbKeymap();

} // namespace contour::input
