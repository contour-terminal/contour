// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyTranslation.hpp>
#include <contour/input/KeyboardPlatform.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace contour::input
{

namespace
{
    using std::array;
    using std::pair;

    /// The character a key produces when the platform reported no text for it: Ctrl+U arrives with none
    /// under the Kitty protocol's disambiguation, and a letter is named by its Qt key, uppercase.
    constexpr auto CharMappings = array {
        // {{{
        // clang-format off
        pair { Qt::Key_Space, ' ' },
        pair { Qt::Key_Exclam, '!' },
        pair { Qt::Key_QuoteDbl, '"' },
        pair { Qt::Key_NumberSign, '#' },
        pair { Qt::Key_Dollar, '$' },
        pair { Qt::Key_Percent, '%' },
        pair { Qt::Key_Ampersand, '&' },
        pair { Qt::Key_Apostrophe, '\'' },
        pair { Qt::Key_ParenLeft, '(' },
        pair { Qt::Key_ParenRight, ')' },
        pair { Qt::Key_Asterisk, '*' },
        pair { Qt::Key_Plus, '+' },
        pair { Qt::Key_Comma, ',' },
        pair { Qt::Key_Minus, '-' },
        pair { Qt::Key_Period, '.' },
        pair { Qt::Key_Slash, '/' },
        pair { Qt::Key_0, '0' },
        pair { Qt::Key_1, '1' },
        pair { Qt::Key_2, '2' },
        pair { Qt::Key_3, '3' },
        pair { Qt::Key_4, '4' },
        pair { Qt::Key_5, '5' },
        pair { Qt::Key_6, '6' },
        pair { Qt::Key_7, '7' },
        pair { Qt::Key_8, '8' },
        pair { Qt::Key_9, '9' },
        pair { Qt::Key_Colon, ':' },
        pair { Qt::Key_Semicolon, ';' },
        pair { Qt::Key_Less, '<' },
        pair { Qt::Key_Equal, '=' },
        pair { Qt::Key_Greater, '>' },
        pair { Qt::Key_Question, '?' },
        pair { Qt::Key_At, '@' },
        pair { Qt::Key_A, 'A' },
        pair { Qt::Key_B, 'B' },
        pair { Qt::Key_C, 'C' },
        pair { Qt::Key_D, 'D' },
        pair { Qt::Key_E, 'E' },
        pair { Qt::Key_F, 'F' },
        pair { Qt::Key_G, 'G' },
        pair { Qt::Key_H, 'H' },
        pair { Qt::Key_I, 'I' },
        pair { Qt::Key_J, 'J' },
        pair { Qt::Key_K, 'K' },
        pair { Qt::Key_L, 'L' },
        pair { Qt::Key_M, 'M' },
        pair { Qt::Key_N, 'N' },
        pair { Qt::Key_O, 'O' },
        pair { Qt::Key_P, 'P' },
        pair { Qt::Key_Q, 'Q' },
        pair { Qt::Key_R, 'R' },
        pair { Qt::Key_S, 'S' },
        pair { Qt::Key_T, 'T' },
        pair { Qt::Key_U, 'U' },
        pair { Qt::Key_V, 'V' },
        pair { Qt::Key_W, 'W' },
        pair { Qt::Key_X, 'X' },
        pair { Qt::Key_Y, 'Y' },
        pair { Qt::Key_Z, 'Z' },
        pair { Qt::Key_BracketLeft, '[' },
        pair { Qt::Key_Backslash, '\\' },
        pair { Qt::Key_BracketRight, ']' },
        pair { Qt::Key_AsciiCircum, '^' },
        pair { Qt::Key_Underscore, '_' },
        pair { Qt::Key_QuoteLeft, '`' },
        pair { Qt::Key_BraceLeft, '{' },
        pair { Qt::Key_Bar, '|' },
        pair { Qt::Key_BraceRight, '}' },
        pair { Qt::Key_AsciiTilde, '~' },
        // clang-format on
    }; // }}}

    /// @return A named-key event carrying @p event's type and origin.
    [[nodiscard]] KeyEvent keyEventOf(NativeKeyEvent const& event,
                                      vtbackend::Key key,
                                      vtbackend::KeyboardModifiers modifiers)
    {
        return KeyEvent { .key = key, .modifiers = modifiers, .type = event.type, .origin = event.origin };
    }

    /// @return A one-character event carrying @p event's type and origin.
    [[nodiscard]] KeyEvent characterEventOf(NativeKeyEvent const& event,
                                            char32_t codepoint,
                                            vtbackend::KeyIdentity identity,
                                            vtbackend::KeyboardModifiers modifiers)
    {
        return KeyEvent { .text = std::u32string(1, codepoint),
                          .identity = identity,
                          .modifiers = modifiers,
                          .type = event.type,
                          .origin = event.origin };
    }

    /// The keypad keys. Qt reports a keypad key as the main-block key with Qt::KeypadModifier.
    [[nodiscard]] std::optional<KeyEvent> keypadKeyOf(NativeKeyEvent const& event,
                                                      vtbackend::KeyboardModifiers modifiers)
    {
        using vtbackend::Key;

        auto const qtModifiers = qtModifiersOf(event.qtModifiers);
        if (!qtModifiers.testFlag(Qt::KeypadModifier))
            return std::nullopt;

        auto const key = event.qtKey;
        std::optional<Key> mappedKey = std::nullopt;
        auto inferredModifiers = modifiers;
        switch (key)
        {
            // Qt reports Key_0..9 with KeypadModifier only when NumLock is ON.
            // When NumLock is OFF, Qt reports navigation keys instead (Insert, End, etc.).
            // So the presence of these key codes definitionally implies NumLock is active.
            case Qt::Key_0:
            case Qt::Key_1:
            case Qt::Key_2:
            case Qt::Key_3:
            case Qt::Key_4:
            case Qt::Key_5:
            case Qt::Key_6:
            case Qt::Key_7:
            case Qt::Key_8:
            case Qt::Key_9:
                mappedKey = static_cast<Key>(static_cast<int>(Key::Numpad_0) + (key - Qt::Key_0));
                inferredModifiers.locks |= vtbackend::LockKey::NumLock;
                break;
            // Operator and Enter keys have the same Qt key code regardless of NumLock state,
            // but produce text when NumLock is active.
            case Qt::Key_Asterisk: mappedKey = Key::Numpad_Multiply; break;
            case Qt::Key_Plus: mappedKey = Key::Numpad_Add; break;
            case Qt::Key_Minus: mappedKey = Key::Numpad_Subtract; break;
            case Qt::Key_Period: mappedKey = Key::Numpad_Decimal; break;
            case Qt::Key_Slash: mappedKey = Key::Numpad_Divide; break;
            case Qt::Key_Enter: mappedKey = Key::Numpad_Enter; break;
            default: break;
        }
        if (!mappedKey)
            return std::nullopt;

        // For operator/Enter keys, infer NumLock from non-empty text
        if (!event.text.empty())
            inferredModifiers.locks |= vtbackend::LockKey::NumLock;
        return keyEventOf(event, *mappedKey, inferredModifiers);
    }
} // namespace

namespace detail
{
    vtbackend::KeyIdentity identityOf(NativeKeyEvent const& event,
                                      KeyboardLayout const& layout,
                                      uint32_t group)
    {
        // What the platform natively calls this key, translated into the two vocabularies the keyboard
        // protocols speak. Both go through the layout rather than being copied across raw: only it knows
        // how to read nativeVirtualKey(), which is a positional Carbon key code on macOS, a keysym on
        // X11 and a VK code on Win32.
        auto const nativeKey = NativeKeyCode { .virtualKey = event.nativeVirtualKey,
                                               .scanCode = event.nativeScanCode,
                                               .group = group };
        return vtbackend::KeyIdentity {
            .unshiftedKey = layout.unshiftedKeyOf(nativeKey),
            .nativeVirtualKey = layout.win32VirtualKeyOf(nativeKey),
        };
    }

    std::optional<KeyEvent> translateKey(NativeKeyEvent const& event,
                                         vtbackend::KeyboardModifiers modifiers,
                                         vtbackend::KeyIdentity identity)
    {
        if (auto named = namedKeyEventOf(event, modifiers))
            return named;
        return unnamedKeyEventOf(event, modifiers, identity);
    }

    std::optional<KeyEvent> namedKeyEventOf(NativeKeyEvent const& event,
                                            vtbackend::KeyboardModifiers modifiers)
    {
        if (auto keypad = keypadKeyOf(event, modifiers))
            return keypad;
        if (auto const named = namedKeyOf(event.qtKey))
            return keyEventOf(event, *named, modifiers);
        return std::nullopt;
    }

    std::optional<KeyEvent> unnamedKeyEventOf(NativeKeyEvent const& event,
                                              vtbackend::KeyboardModifiers modifiers,
                                              vtbackend::KeyIdentity identity)
    {
        if (event.text.empty())
        {
            if (auto const i = std::ranges::find(
                    CharMappings, event.qtKey, [](auto const& x) { return static_cast<int>(x.first); });
                i != CharMappings.end())
                return characterEventOf(event, static_cast<char32_t>(i->second), identity, modifiers);
        }

        if (event.qtKey == Qt::Key_Backtab)
            return keyEventOf(event, vtbackend::Key::Tab, modifiers.with(vtbackend::Modifier::Shift));

        return std::nullopt;
    }

    std::optional<KeyEvent> controlCharacterOf(NativeKeyEvent const& event,
                                               vtbackend::KeyboardModifiers modifiers,
                                               vtbackend::KeyIdentity identity)
    {
        if (0x20 <= event.qtKey && event.qtKey < 0x80 && modifiers.chord.test(vtbackend::Modifier::Control))
            return characterEventOf(event, static_cast<char32_t>(event.qtKey), identity, modifiers);
        return std::nullopt;
    }

    KeyEvent textOf(NativeKeyEvent const& event,
                    vtbackend::KeyboardModifiers modifiers,
                    vtbackend::KeyIdentity identity)
    {
        return KeyEvent { .text = event.text,
                          .identity = identity,
                          .modifiers = modifiers,
                          .type = event.type,
                          .origin = event.origin };
    }

    KeyEvent translateEvent(NativeKeyEvent const& event,
                            vtbackend::KeyboardModifiers modifiers,
                            KeyboardLayout const& layout,
                            uint32_t group)
    {
        auto const identity = identityOf(event, layout, group);
        if (auto translated = translateKey(event, modifiers, identity))
            return *std::move(translated);
        if (auto control = controlCharacterOf(event, modifiers, identity))
            return *std::move(control);
        return textOf(event, modifiers, identity);
    }

    KeyEvent inputMethodCommitOf(NativeKeyEvent const& event, vtbackend::LockKeys locks)
    {
        return KeyEvent { .text = event.text,
                          .modifiers = vtbackend::KeyboardModifiers { locks },
                          .type = vtbackend::KeyboardEventType::Press,
                          .origin = KeyEventOrigin::InputMethodCommit };
    }
} // namespace detail

KeyboardPolicy keyboardPolicyOf(config::TerminalProfile const& profile)
{
    return KeyboardPolicy { .optionAsAlt = profile.optionAsAlt.value() };
}

// Known limitation: the press is rebuilt from the QML KeyEvent, which exposes no nativeModifiers, so they
// are zero here. A chrome lookup on macOS therefore cannot tell which Option key is held and treats it as
// the platform treats an unnamed side under option_as_alt left or right.
vtbackend::Modifiers chordOfKeyPress(KeyboardPlatform const& platform,
                                     int qtKey,
                                     uint32_t qtModifiers,
                                     std::u32string_view text)
{
    return platform
        .normalize(
            NativeKeyEvent { .qtKey = qtKey, .text = std::u32string { text }, .qtModifiers = qtModifiers })
        .modifiers.chord;
}

std::unique_ptr<KeyboardPlatform> makeKeyboardPlatform(config::KeyboardPlatformKind kind,
                                                       KeyboardPolicy policy,
                                                       KeyboardLayout const& layout)
{
    switch (kind)
    {
        case config::KeyboardPlatformKind::Xkb: return detail::makeXkbKeyboardPlatform(layout);
        case config::KeyboardPlatformKind::Win32: return detail::makeWin32KeyboardPlatform(layout);
        case config::KeyboardPlatformKind::Mac: return detail::makeMacKeyboardPlatform(policy, layout);
    }
    return detail::makeXkbKeyboardPlatform(layout);
}

KeyboardPlatformFactory makeKeyboardPlatformFactory(config::KeyboardPlatformKind kind,
                                                    KeyboardLayout const& layout)
{
    return [kind, &layout](KeyboardPolicy policy) {
        return makeKeyboardPlatform(kind, policy, layout);
    };
}

} // namespace contour::input
