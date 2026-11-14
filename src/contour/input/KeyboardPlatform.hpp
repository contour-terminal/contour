// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigFwd.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/KeyEvent.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/NativeKeyEvent.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace contour::input
{

/// Whether the platform input method sees a keystroke.
enum class InputMethodRoute : uint8_t
{
    ToInputMethod = 0, //!< The input method sees it, and may compose with it.
    Bypass,            //!< It reaches the terminal as a key event, past the input method.
};

/// The bit of macOS's device-dependent modifier flags (`NX_DEVICELALTKEYMASK`) set while the left Option key
/// is down. Qt hands the flags over as QKeyEvent::nativeModifiers().
inline constexpr uint32_t MacLeftOptionBit = 0x00000020;

/// The bit of macOS's device-dependent modifier flags (`NX_DEVICERALTKEYMASK`) set while the right Option key
/// is down.
inline constexpr uint32_t MacRightOptionBit = 0x00000040;

/// The device-independent modifier flags of macOS (`NSEventModifierFlag*`), as QKeyEvent::nativeModifiers()
/// carries them.
inline constexpr uint32_t MacShiftBit = 0x00020000;   ///< `NSEventModifierFlagShift`.
inline constexpr uint32_t MacControlBit = 0x00040000; ///< `NSEventModifierFlagControl`.
inline constexpr uint32_t MacOptionBit = 0x00080000;  ///< `NSEventModifierFlagOption`.
inline constexpr uint32_t MacCommandBit = 0x00100000; ///< `NSEventModifierFlagCommand`.

/// The virtual key codes (`kVK_*`) of the arrow keys on macOS, as QKeyEvent::nativeVirtualKey() carries them.
inline constexpr uint32_t MacKeyLeftArrow = 0x7b;  ///< `kVK_LeftArrow`.
inline constexpr uint32_t MacKeyRightArrow = 0x7c; ///< `kVK_RightArrow`.
inline constexpr uint32_t MacKeyDownArrow = 0x7d;  ///< `kVK_DownArrow`.

/// A key press as the platform names it: the native modifier flags and the native virtual key together, so
/// that the two same-typed values cannot be passed the wrong way round.
struct NativeChord
{
    uint32_t modifiers = 0;  ///< QKeyEvent::nativeModifiers(); in a table row, exactly the Shift, Control,
                             ///< Option and Command flags held.
    uint32_t virtualKey = 0; ///< QKeyEvent::nativeVirtualKey().
};

/// The chords the input context swallows: StandardKeyBinding.dict maps them to text writing-direction
/// commands (makeBaseWritingDirection*, makeTextWritingDirection*), and Qt (QNSView) then delivers no key
/// press for them at all, so a terminal keybinding on one never fires. One row per chord.
///
/// WORKAROUND for that Qt bug, tracked in contour-terminal/contour#2356: remove this table, its use in
/// MacKeyboardPlatform::inputMethodRouteFor(), and the CocoaKeyDelivery test once Qt is fixed.
inline constexpr std::array MacNativeChords {
    NativeChord { .modifiers = MacControlBit | MacCommandBit, .virtualKey = MacKeyDownArrow },
    NativeChord { .modifiers = MacControlBit | MacCommandBit, .virtualKey = MacKeyLeftArrow },
    NativeChord { .modifiers = MacControlBit | MacCommandBit, .virtualKey = MacKeyRightArrow },
    NativeChord { .modifiers = MacControlBit | MacOptionBit | MacCommandBit, .virtualKey = MacKeyDownArrow },
    NativeChord { .modifiers = MacControlBit | MacOptionBit | MacCommandBit, .virtualKey = MacKeyLeftArrow },
    NativeChord { .modifiers = MacControlBit | MacOptionBit | MacCommandBit, .virtualKey = MacKeyRightArrow },
};

/// Whether macOS's input context swallows a key press (see MacNativeChords; #2356).
/// @param chord The key press: the arrows carry Function and NumericPad, and every key may carry
///        device-dependent bits, none of which a row names, so only Shift, Control, Option and Command count.
/// @return True where the chord is a row of the table.
[[nodiscard]] constexpr bool macInputContextSwallows(NativeChord const& chord) noexcept
{
    auto constexpr Compared = MacShiftBit | MacControlBit | MacOptionBit | MacCommandBit;
    return std::ranges::any_of(MacNativeChords, [&](NativeChord const& row) {
        return row.virtualKey == chord.virtualKey && row.modifiers == (chord.modifiers & Compared);
    });
}

/// Which Option keys are down, as macOS's device-dependent modifier flags report them.
/// A pure function of the bits on every host, so that it is tested everywhere; only the macOS normalizer
/// consults it. The device-independent Option flag (`0x80000`) says Option is held but not which one, and
/// is ignored here.
/// @param nativeModifiers The value of QKeyEvent::nativeModifiers().
/// @return The keys whose bit is set; None when neither is.
[[nodiscard]] constexpr config::OptionKeySide optionKeySideOf(uint32_t nativeModifiers) noexcept
{
    auto bits = uint8_t { 0 };
    if ((nativeModifiers & MacLeftOptionBit) != 0)
        bits |= static_cast<uint8_t>(config::OptionKeySide::Left);
    if ((nativeModifiers & MacRightOptionBit) != 0)
        bits |= static_cast<uint8_t>(config::OptionKeySide::Right);
    return static_cast<config::OptionKeySide>(bits);
}

/// The bit `captureKeyEvent` ORs into NativeKeyEvent::nativeModifiers while CapsLock is toggled on, on
/// Windows. The capture boundary samples the toggles itself rather than trusting Qt's mask, and keeps Qt's
/// own bits intact. Qt's Windows key mapper uses the low bits (left/right Shift, Control, Alt, Meta), 0x100
/// to 0x400 (lock keys) and 0x01000000 (ExtendedKey), so the top two bits never collide with them.
inline constexpr uint32_t Win32CapsLockBit = 0x40000000;

/// The bit `captureKeyEvent` ORs into NativeKeyEvent::nativeModifiers while NumLock is toggled on, on
/// Windows.
inline constexpr uint32_t Win32NumLockBit = 0x80000000;

/// The lock keys a Windows key event says are toggled on.
/// @param nativeModifiers The value of NativeKeyEvent::nativeModifiers, after the capture boundary sampled
/// it.
/// @return CapsLock and NumLock, as their sampled bits say.
[[nodiscard]] constexpr vtbackend::LockKeys win32LockKeysOf(uint32_t nativeModifiers) noexcept
{
    auto locks = vtbackend::LockKeys {};
    if ((nativeModifiers & Win32CapsLockBit) != 0)
        locks |= vtbackend::LockKey::CapsLock;
    if ((nativeModifiers & Win32NumLockBit) != 0)
        locks |= vtbackend::LockKey::NumLock;
    return locks;
}

/// What a Ctrl+Alt combination means for one event. Windows reports the AltGr key as Ctrl+Alt, so the same
/// two modifiers are either a chord the user pressed or a keyboard-layout level that selected the character.
enum class CtrlAltRole : uint8_t
{
    Chord = 0, //!< Ctrl and Alt are modifiers, reported as such.
    AltGr,     //!< Ctrl and Alt selected the AltGr level; they are not modifiers of the result.
};

/// Classifies a key event's Ctrl+Alt the way Windows itself does: the AltGr level yields graphical
/// characters, while a genuine Ctrl+Alt chord yields none (or a control character). This is Windows
/// Terminal's heuristic for the same ambiguity.
/// @param qtModifiers The event's Qt::KeyboardModifiers bits.
/// @param text The text the platform produced for the event.
/// @return AltGr when Ctrl and Alt are both held and @p text is non-empty and wholly graphical, else Chord.
[[nodiscard]] CtrlAltRole ctrlAltRoleOf(uint32_t qtModifiers, std::u32string_view text) noexcept;

/// The user's settings a keyboard platform decodes events under. Fixed for a platform's life: a new
/// policy is a new platform (see KeyboardPlatformFactory).
struct KeyboardPolicy
{
    config::OptionAsAlt optionAsAlt = config::OptionAsAlt::Off; ///< Which Option keys are Alt (macOS).
};

/// @param profile The profile whose keyboard settings are wanted.
/// @return The policy @p profile asks keys to be decoded under.
[[nodiscard]] KeyboardPolicy keyboardPolicyOf(config::TerminalProfile const& profile);

/// What a platform means by a key event: the one place a platform's conventions are decoded.
///
/// Everything downstream of it -- the router, the bindings, the encoders -- is platform-free. Every
/// implementation compiles on every host and is picked at runtime by makeKeyboardPlatform(), so each
/// platform's rules are tested on any machine.
class KeyboardPlatform
{
  public:
    virtual ~KeyboardPlatform() = default;

    /// Canonicalizes one native event.
    /// @param event The event as captured.
    /// @return The event as the platform means it; KeyEvent::isUnhandled() when it means nothing here.
    [[nodiscard]] virtual KeyEvent normalize(NativeKeyEvent const& event) const = 0;

    /// Whether the platform input method should see a keystroke.
    ///
    /// On macOS that is not so for an Option key acting as Alt, which must not start a dead key, nor for the
    /// Control+Command arrow chords the input context swallows (contour-terminal/contour#2356).
    /// @param chord The keystroke's QKeyEvent::nativeModifiers() and QKeyEvent::nativeVirtualKey().
    /// @return Bypass where the input method must not see the keystroke.
    [[nodiscard]] virtual InputMethodRoute inputMethodRouteFor(NativeChord const& chord) const = 0;
};

/// The chord a key press holds, decided as @p platform decides it for the terminal -- so that AltGr on
/// Windows, or a composing Option key on macOS, is no more a chord for a lookup than it is for the program.
/// @param platform Decodes the press.
/// @param qtKey The Qt::Key.
/// @param qtModifiers The Qt::KeyboardModifiers bits.
/// @param text The text the press produced.
/// @return The chord the platform keeps for the press, which is empty only when it holds no modifier the
///         platform counts (a composing Option key or AltGr leaves none); a text-typing press keeps its
///         modifiers.
[[nodiscard]] vtbackend::Modifiers chordOfKeyPress(KeyboardPlatform const& platform,
                                                   int qtKey,
                                                   uint32_t qtModifiers,
                                                   std::u32string_view text);

/// Builds the keyboard platform of one kind.
/// @param kind Whose conventions the events speak.
/// @param policy The user's settings to decode them under.
/// @param layout Resolves native key codes; held by reference, so it must outlive the platform.
/// @return The platform.
[[nodiscard]] std::unique_ptr<KeyboardPlatform> makeKeyboardPlatform(config::KeyboardPlatformKind kind,
                                                                     KeyboardPolicy policy,
                                                                     KeyboardLayout const& layout);

/// Builds a keyboard platform for a policy. A session holds one and calls it again whenever its profile
/// changes the policy, because a platform is configured once, at construction.
using KeyboardPlatformFactory = std::function<std::unique_ptr<KeyboardPlatform>(KeyboardPolicy)>;

/// @param kind Whose conventions the platforms decode.
/// @param layout Resolves native key codes; held by reference, so it must outlive every platform built.
/// @return A factory building platforms of @p kind over @p layout.
[[nodiscard]] KeyboardPlatformFactory makeKeyboardPlatformFactory(config::KeyboardPlatformKind kind,
                                                                  KeyboardLayout const& layout);

} // namespace contour::input
