// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyTranslation.hpp>
#include <contour/input/KeyboardPlatform.hpp>

#include <QtCore/QChar>

#include <algorithm>

namespace contour::input
{

namespace
{
    /// The first codepoint beyond the Basic Multilingual Plane. What Qt delivered as a surrogate pair is one
    /// such codepoint in a UTF-32 text, and a graphical one: nothing there is a control character.
    constexpr char32_t FirstSupplementaryCodepoint = 0x10000;

    /// Windows: the lock toggles are sampled by the capture boundary into the Win32 lock bits, and the
    /// AltGr key arrives as Ctrl+Alt -- a layout level, which must not reach the encoders as modifiers, or
    /// they would Ctrl-map an AltGr '[' into ESC.
    ///
    /// Nothing in here allocates: it runs on every keystroke.
    class Win32KeyboardPlatform final: public KeyboardPlatform
    {
      public:
        explicit Win32KeyboardPlatform(KeyboardLayout const& layout): _layout { layout } {}

        [[nodiscard]] KeyEvent normalize(NativeKeyEvent const& event) const override
        {
            if (event.origin == KeyEventOrigin::InputMethodCommit)
                return detail::inputMethodCommitOf(event, win32LockKeysOf(event.nativeModifiers));

            auto chord = chordOf(Qt::KeyboardModifiers::fromInt(static_cast<int>(event.qtModifiers)));
            if (ctrlAltRoleOf(event.qtModifiers, event.text) == CtrlAltRole::AltGr)
                chord = chord.without(AltGrEquivalent);

            auto const modifiers =
                vtbackend::KeyboardModifiers { chord, win32LockKeysOf(event.nativeModifiers) };
            return detail::translateEvent(event, modifiers, _layout);
        }

        /// Every keystroke reaches the input method on Windows: no modifier there means Alt on one side and
        /// composes on the other.
        [[nodiscard]] InputMethodRoute inputMethodRouteFor(uint32_t /*nativeModifiers*/) const override
        {
            return InputMethodRoute::ToInputMethod;
        }

      private:
        static constexpr auto AltGrEquivalent =
            vtbackend::Modifiers { vtbackend::Modifier::Alt, vtbackend::Modifier::Control };

        KeyboardLayout const& _layout;
    };
} // namespace

CtrlAltRole ctrlAltRoleOf(uint32_t qtModifiers, std::u32string_view text) noexcept
{
    constexpr auto CtrlAltBits =
        static_cast<uint32_t>(Qt::ControlModifier) | static_cast<uint32_t>(Qt::AltModifier);
    if ((qtModifiers & CtrlAltBits) != CtrlAltBits)
        return CtrlAltRole::Chord;

    // A control character -- Ctrl+Alt+Esc yields 0x1B -- is a chord's product, not a layout's. A layout may
    // yield several characters (a ligature, or a dead key that did not combine).
    auto const isGraphical = [](char32_t ch) {
        return ch >= FirstSupplementaryCodepoint || (QChar::isPrint(ch) && ch != U' ');
    };
    return !text.empty() && std::ranges::all_of(text, isGraphical) ? CtrlAltRole::AltGr : CtrlAltRole::Chord;
}

std::unique_ptr<KeyboardPlatform> detail::makeWin32KeyboardPlatform(KeyboardLayout const& layout)
{
    return std::make_unique<Win32KeyboardPlatform>(layout);
}

} // namespace contour::input
