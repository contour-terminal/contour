// SPDX-License-Identifier: Apache-2.0
//
// The translation steps every KeyboardPlatform shares, once it has decoded an event's modifiers in its own
// way. Internal to contour_input: a normalizer composes these, nothing else calls them.
#pragma once

#include <contour/input/KeyEvent.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/input/NativeKeyEvent.hpp>

#include <memory>
#include <optional>

namespace contour::input::detail
{

/// The key's layout-resolved identity: what it produces unmodified, and its Win32 VK code.
/// @param event The native event.
/// @param layout Reads the event's native key code.
/// @return The identity.
[[nodiscard]] vtbackend::KeyIdentity identityOf(NativeKeyEvent const& event, KeyboardLayout const& layout);

/// The steps that do not depend on how the platform treats characters: the keypad keys, the named keys,
/// a text-less key the Qt key → character table names, and Backtab -- in that order, as the first that
/// applies wins.
/// @param event The native event.
/// @param modifiers The modifiers the platform decoded.
/// @param identity The key's identity (identityOf()).
/// @return The event, or nullopt when none of the steps applies.
[[nodiscard]] std::optional<KeyEvent> translateKey(NativeKeyEvent const& event,
                                                   vtbackend::KeyboardModifiers modifiers,
                                                   vtbackend::KeyIdentity identity);

/// translateKey()'s first half: the keypad keys and the named keys.
/// @return The event, or nullopt when the key is neither.
[[nodiscard]] std::optional<KeyEvent> namedKeyEventOf(NativeKeyEvent const& event,
                                                      vtbackend::KeyboardModifiers modifiers);

/// translateKey()'s second half: a text-less key the Qt key → character table names, and Backtab.
/// @return The event, or nullopt when the key is neither.
[[nodiscard]] std::optional<KeyEvent> unnamedKeyEventOf(NativeKeyEvent const& event,
                                                        vtbackend::KeyboardModifiers modifiers,
                                                        vtbackend::KeyIdentity identity);

/// The Ctrl+ASCII rule: with Control held, a key in the ASCII range is the character its Qt key names
/// (uppercase for a letter), whatever text the platform produced for it.
/// @return The event, or nullopt when Control is not held or the key is not ASCII.
[[nodiscard]] std::optional<KeyEvent> controlCharacterOf(NativeKeyEvent const& event,
                                                         vtbackend::KeyboardModifiers modifiers,
                                                         vtbackend::KeyIdentity identity);

/// The text route: the characters the platform produced, each under @p modifiers.
/// @return The event; unhandled when the platform produced no text.
[[nodiscard]] KeyEvent textOf(NativeKeyEvent const& event,
                              vtbackend::KeyboardModifiers modifiers,
                              vtbackend::KeyIdentity identity);

/// Text an input method committed: with no chord and no key identity, as a press -- but with the lock
/// keys the capture boundary sampled for it, as a key typing the same text would carry.
/// @param event An event of origin KeyEventOrigin::InputMethodCommit.
/// @param locks The lock keys the platform decodes from the event's native modifiers.
/// @return The text-only event.
[[nodiscard]] KeyEvent inputMethodCommitOf(NativeKeyEvent const& event, vtbackend::LockKeys locks);

/// Every shared step in order -- translateKey(), the Ctrl+ASCII rule, then the text route -- for a platform
/// that adds no character rule of its own.
/// @param event The native event, of origin KeyEventOrigin::Keyboard.
/// @param modifiers The modifiers the platform decoded.
/// @param layout Reads the event's native key code.
/// @return The event; unhandled when no step applies.
[[nodiscard]] KeyEvent translateEvent(NativeKeyEvent const& event,
                                      vtbackend::KeyboardModifiers modifiers,
                                      KeyboardLayout const& layout);

/// @param policy Which Option keys are Alt.
/// @param layout Resolves native key codes; held by reference.
/// @return The macOS normalizer.
[[nodiscard]] std::unique_ptr<KeyboardPlatform> makeMacKeyboardPlatform(KeyboardPolicy policy,
                                                                        KeyboardLayout const& layout);

/// @param layout Resolves native key codes; held by reference.
/// @return The X11/Wayland normalizer.
[[nodiscard]] std::unique_ptr<KeyboardPlatform> makeXkbKeyboardPlatform(KeyboardLayout const& layout);

} // namespace contour::input::detail
