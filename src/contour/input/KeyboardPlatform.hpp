// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigFwd.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/KeyEvent.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/NativeKeyEvent.hpp>

#include <cstdint>
#include <functional>
#include <memory>

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

    /// Whether the platform input method should see a keystroke held with these modifiers.
    /// @param nativeModifiers The value of QKeyEvent::nativeModifiers().
    /// @return Bypass where a held key is one the input method must not compose with.
    [[nodiscard]] virtual InputMethodRoute inputMethodRouteFor(uint32_t nativeModifiers) const = 0;
};

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
