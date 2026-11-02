// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyTranslation.hpp>
#include <contour/input/KeyboardPlatform.hpp>

namespace contour::input
{

namespace
{
    /// X11 and Wayland: the chord is Qt's, the lock keys ride in the XCB core modifier mask, and AltGr is a
    /// key of its own -- it selects a level and never reaches Qt as Ctrl+Alt -- so no chord is reinterpreted.
    class XkbKeyboardPlatform final: public KeyboardPlatform
    {
      public:
        explicit XkbKeyboardPlatform(KeyboardLayout const& layout): _layout { layout } {}

        [[nodiscard]] KeyEvent normalize(NativeKeyEvent const& event) const override
        {
            if (event.origin == KeyEventOrigin::InputMethodCommit)
                return detail::inputMethodCommitOf(event, xcbLockKeysOf(event.nativeModifiers));

            auto const modifiers = vtbackend::KeyboardModifiers {
                chordOf(Qt::KeyboardModifiers::fromInt(static_cast<int>(event.qtModifiers))),
                xcbLockKeysOf(event.nativeModifiers),
            };
            return detail::translateEvent(event, modifiers, _layout, xcbGroupOf(event.nativeModifiers));
        }

        /// Nothing on X11 or Wayland bypasses the input method: no modifier there means Alt on one side and
        /// composes on the other.
        [[nodiscard]] InputMethodRoute inputMethodRouteFor(uint32_t /*nativeModifiers*/) const override
        {
            return InputMethodRoute::ToInputMethod;
        }

      private:
        KeyboardLayout const& _layout;
    };
} // namespace

std::unique_ptr<KeyboardPlatform> detail::makeXkbKeyboardPlatform(KeyboardLayout const& layout)
{
    return std::make_unique<XkbKeyboardPlatform>(layout);
}

} // namespace contour::input
