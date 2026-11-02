// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/NativeKeyEvent.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace contour::input
{

/// Whether a chord holds the platform's application modifier: the one the platform reserves for
/// shortcuts rather than passing to the program in the terminal (⌘ on macOS). No other platform has one,
/// so there every event is Absent and Super reaches applications as any modifier does.
enum class AppModifier : uint8_t
{
    Absent = 0, //!< The chord does not hold it.
    Held,       //!< The chord holds it.
};

/// A key event as the platform means it: what was pressed, in the vocabulary the router and the encoders
/// speak, with every platform convention already decoded.
///
/// It is either a named key (`key` set), or the characters in `text`, each routed as a character event in
/// order. An event with neither is one the platform could not translate, which Qt routes on.
struct KeyEvent
{
    std::optional<vtbackend::Key> key {};      ///< Named key; nullopt for a character event.
    std::u32string text {};                    ///< Codepoints routed as character events, in order.
    vtbackend::KeyIdentity identity {};        ///< The key's layout-resolved identity, for character events.
    vtbackend::KeyboardModifiers modifiers {}; ///< The semantic chord, and the latched lock keys.
    config::OptionKeySide altSide = config::OptionKeySide::None; ///< The Option keys that were down.
    AppModifier appModifier = AppModifier::Absent;               ///< Whether ⌘ (or its kind) was held.
    vtbackend::KeyboardEventType type = vtbackend::KeyboardEventType::Press;
    KeyEventOrigin origin = KeyEventOrigin::Keyboard;

    /// @return True when the event is neither a key nor text: the platform could not translate it.
    [[nodiscard]] bool isUnhandled() const noexcept { return !key && text.empty(); }
};

} // namespace contour::input
