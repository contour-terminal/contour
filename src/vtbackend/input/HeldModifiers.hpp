// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/input/InputGenerator.hpp>

#include <algorithm>
#include <array>

namespace vtbackend
{

/// Which chord modifier a modifier key is.
struct ModifierKey
{
    Key key;           ///< The modifier key.
    Modifier modifier; ///< The chord modifier it holds.
};

/// Every modifier key that holds a chord modifier, left and right alike.
inline constexpr auto ModifierKeys = std::array {
    ModifierKey { Key::LeftShift, Modifier::Shift },     ModifierKey { Key::RightShift, Modifier::Shift },
    ModifierKey { Key::LeftControl, Modifier::Control }, ModifierKey { Key::RightControl, Modifier::Control },
    ModifierKey { Key::LeftAlt, Modifier::Alt },         ModifierKey { Key::RightAlt, Modifier::Alt },
    ModifierKey { Key::LeftSuper, Modifier::Super },     ModifierKey { Key::RightSuper, Modifier::Super },
    ModifierKey { Key::LeftHyper, Modifier::Hyper },     ModifierKey { Key::RightHyper, Modifier::Hyper },
    ModifierKey { Key::LeftMeta, Modifier::Meta },       ModifierKey { Key::RightMeta, Modifier::Meta },
};

/// The chord held once a key event is applied. Platforms disagree on whether a modifier key's own press
/// already carries it (X11 reports it on the release, Wayland on the press), so a modifier key's own modifier
/// is added on a press and removed on a release, whatever the chord says.
/// @param key The key of the event.
/// @param eventType Press, repeat or release.
/// @param chord The chord the event reported.
/// @return The chord held after it.
[[nodiscard]] constexpr Modifiers heldAfter(Key key, KeyboardEventType eventType, Modifiers chord) noexcept
{
    auto const found = std::ranges::find(ModifierKeys, key, &ModifierKey::key);
    if (found == ModifierKeys.end())
        return chord;
    switch (eventType)
    {
        case KeyboardEventType::Press:
        case KeyboardEventType::Repeat: return chord | found->modifier;
        case KeyboardEventType::Release: return chord.without(found->modifier);
    }
    return chord;
}

/// @p chord as a mouse binding matches it: without the mouse-protocol bypass modifiers, when it holds all of
/// them, since those only say the press is not the application's.
///
/// The one rule both the click (which binding runs) and the reveal (which chord shows links) go by, so that a
/// link is revealed exactly when a click would follow it.
/// @param chord The chord held.
/// @param bypass The modifiers that hand a press back to the terminal (usually Shift).
/// @return The chord to match against a binding's modifiers.
[[nodiscard]] constexpr Modifiers bindingModifiers(Modifiers chord, Modifiers bypass) noexcept
{
    return chord.contains(bypass) ? chord.without(bypass) : chord;
}

} // namespace vtbackend
