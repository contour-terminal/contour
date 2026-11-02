// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyTranslation.hpp>
#include <contour/input/KeyboardPlatform.hpp>

#include <cctype>

namespace contour::input
{

namespace
{
    /// macOS: ⌘ is the application modifier, CapsLock rides in the NSEvent flags, and one rule decides what
    /// a held Option key is.
    ///
    /// The Option rule: an Option key that `option_as_alt` names is Alt, on every key. One it does not name
    /// is Alt only where its layer produces no text -- the named keys, and a key that typed nothing -- and
    /// on a key that composes (Option+e, Option+1 → ¡) it composes: the chord drops Alt and the composed
    /// text is sent. So ⌥←/⌥→ and Option+Enter keep working with `option_as_alt` off, and é/¡ keep typing.
    class MacKeyboardPlatform final: public KeyboardPlatform
    {
      public:
        MacKeyboardPlatform(KeyboardPolicy policy, KeyboardLayout const& layout):
            _policy { policy }, _layout { layout }
        {
        }

        [[nodiscard]] KeyEvent normalize(NativeKeyEvent const& event) const override
        {
            if (event.origin == KeyEventOrigin::InputMethodCommit)
                return detail::inputMethodCommitOf(event, macLockKeysOf(event.nativeModifiers));

            auto const modifiers = vtbackend::KeyboardModifiers {
                chordOf(Qt::KeyboardModifiers::fromInt(static_cast<int>(event.qtModifiers))),
                macLockKeysOf(event.nativeModifiers),
            };
            auto const altSide = optionKeySideOf(event.nativeModifiers);
            auto result = translate(event, modifiers, altSide);
            result.altSide = altSide;
            result.appModifier =
                modifiers.chord.test(vtbackend::Modifier::Super) ? AppModifier::Held : AppModifier::Absent;
            return result;
        }

        [[nodiscard]] InputMethodRoute inputMethodRouteFor(uint32_t nativeModifiers) const override
        {
            auto const down = optionKeySideOf(nativeModifiers);
            if (down == config::OptionKeySide::None)
                return InputMethodRoute::ToInputMethod;
            return config::optionKeyRoleForHeldChord(_policy.optionAsAlt, down) == config::OptionKeyRole::Alt
                       ? InputMethodRoute::Bypass
                       : InputMethodRoute::ToInputMethod;
        }

      private:
        [[nodiscard]] KeyEvent translate(NativeKeyEvent const& event,
                                         vtbackend::KeyboardModifiers modifiers,
                                         config::OptionKeySide altSide) const
        {
            auto const identity = detail::identityOf(event, _layout);

            // Keypad and named keys: the Option layer produces no text there, so Alt stays.
            if (auto named = detail::namedKeyEventOf(event, modifiers))
                return *std::move(named);

            auto const optionIsAlt = modifiers.chord.test(vtbackend::Modifier::Alt)
                                     && config::optionKeyRoleForHeldChord(_policy.optionAsAlt, altSide)
                                            == config::OptionKeyRole::Alt;

            // A named Option key is Alt: the character is the Qt key's, not what the Option layer composed --
            // nor nothing, where the layer made the key a dead key.
            // CapsLock inverts the effect of Shift on letter case, which is the one thing a lock key
            // legitimately decides here.
            if (optionIsAlt && 0x20 <= event.qtKey && event.qtKey < 0x80)
            {
                auto const shifted = modifiers.chord.test(vtbackend::Modifier::Shift)
                                     != modifiers.locks.test(vtbackend::LockKey::CapsLock);
                auto const ch =
                    static_cast<char32_t>(shifted ? std::toupper(event.qtKey) : std::tolower(event.qtKey));
                return KeyEvent { .text = std::u32string(1, ch),
                                  .identity = identity,
                                  .modifiers = modifiers,
                                  .type = event.type,
                                  .origin = event.origin };
            }

            // A text-less key: whatever Option key was held made it type nothing, so Alt stays.
            if (auto unnamed = detail::unnamedKeyEventOf(event, modifiers, identity))
                return *std::move(unnamed);

            if (auto control = detail::controlCharacterOf(event, modifiers, identity))
                return *std::move(control);

            // Text an Option key that is not Alt composed: the chord carries no Alt.
            auto const composed = modifiers.chord.test(vtbackend::Modifier::Alt) && !optionIsAlt;
            return detail::textOf(
                event, composed ? modifiers.without(vtbackend::Modifier::Alt) : modifiers, identity);
        }

        KeyboardPolicy _policy;
        KeyboardLayout const& _layout;
    };
} // namespace

std::unique_ptr<KeyboardPlatform> detail::makeMacKeyboardPlatform(KeyboardPolicy policy,
                                                                  KeyboardLayout const& layout)
{
    return std::make_unique<MacKeyboardPlatform>(policy, layout);
}

} // namespace contour::input
