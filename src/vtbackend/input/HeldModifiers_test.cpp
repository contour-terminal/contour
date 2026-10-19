// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/HeldModifiers.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("HeldModifiers.aModifierKeysOwnEventDecides", "[links][input]")
{
    auto const ctrl = Modifiers { Modifier::Control };
    // X11 reports a modifier's own press without it, Wayland with it: both mean "now held".
    CHECK(heldAfter(Key::LeftControl, KeyboardEventType::Press, Modifiers {}) == ctrl);
    CHECK(heldAfter(Key::LeftControl, KeyboardEventType::Press, ctrl) == ctrl);
    CHECK(heldAfter(Key::RightControl, KeyboardEventType::Repeat, Modifiers {}) == ctrl);
    // ...and its release with it, or without it: both mean "now released".
    CHECK(heldAfter(Key::LeftControl, KeyboardEventType::Release, ctrl) == Modifiers {});
    CHECK(heldAfter(Key::LeftControl, KeyboardEventType::Release, Modifiers {}) == Modifiers {});
    // Other chord modifiers survive a modifier key's own event.
    auto const shiftCtrl = Modifiers { Modifier::Shift } | Modifier::Control;
    CHECK(heldAfter(Key::LeftControl, KeyboardEventType::Release, shiftCtrl)
          == Modifiers { Modifier::Shift });
    // Other keys leave the chord as reported.
    CHECK(heldAfter(Key::Enter, KeyboardEventType::Press, ctrl) == ctrl);
}

TEST_CASE("HeldModifiers.everyModifierKeyMapsToItsModifier", "[links][input]")
{
    for (auto const& [key, modifier]: ModifierKeys)
    {
        CHECK(heldAfter(key, KeyboardEventType::Press, Modifiers {}) == Modifiers { modifier });
        CHECK(heldAfter(key, KeyboardEventType::Release, Modifiers { modifier }) == Modifiers {});
    }
}

TEST_CASE("HeldModifiers.bindingModifiersRemovesTheBypassOnlyWhenHeldWhole", "[links][input]")
{
    auto const shift = Modifiers { Modifier::Shift };
    auto const ctrl = Modifiers { Modifier::Control };
    auto const ctrlShift = ctrl | Modifier::Shift;
    CHECK(bindingModifiers(ctrl, shift) == ctrl);
    CHECK(bindingModifiers(ctrlShift, shift) == ctrl);
    CHECK(bindingModifiers(shift, shift) == Modifiers {});
    CHECK(bindingModifiers(Modifiers { Modifier::Alt }, shift) == Modifiers { Modifier::Alt });
    // A bypass of two modifiers is removed only when both are held.
    auto const ctrlAlt = ctrl | Modifier::Alt;
    CHECK(bindingModifiers(ctrl, ctrlAlt) == ctrl);
    CHECK(bindingModifiers(ctrlAlt | Modifier::Shift, ctrlAlt) == Modifiers { Modifier::Shift });
    // No bypass modifier configured: nothing to remove.
    CHECK(bindingModifiers(ctrlShift, Modifiers {}) == ctrlShift);
}
