// SPDX-License-Identifier: Apache-2.0
#include <contour/KeyboardLayout.h>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>

#ifdef __APPLE__
    #include <type_traits>

    #include <Carbon/Carbon.h>
#endif

namespace contour
{

namespace
{
    struct MacKeyCode
    {
        uint32_t keyCode;
        char32_t codepoint;
    };

    /// The Carbon virtual key codes that carry a character on a US ANSI keyboard, sorted by code so
    /// the lookup can bisect. Keys that produce no character — modifiers, function keys, arrows —
    /// are absent and resolve to 0.
    ///
    /// Deliberately spelled out rather than derived: `kVK_ANSI_*` is a positional numbering with no
    /// arithmetic relationship to what the keys produce (A is 0x00, S is 0x01, D is 0x02), so there
    /// is nothing to compute. Adding a key is adding a row.
    constexpr auto MacUsAnsiKeyCodes = std::array {
        MacKeyCode { .keyCode = 0x00, .codepoint = U'a' },    // kVK_ANSI_A
        MacKeyCode { .keyCode = 0x01, .codepoint = U's' },    // kVK_ANSI_S
        MacKeyCode { .keyCode = 0x02, .codepoint = U'd' },    // kVK_ANSI_D
        MacKeyCode { .keyCode = 0x03, .codepoint = U'f' },    // kVK_ANSI_F
        MacKeyCode { .keyCode = 0x04, .codepoint = U'h' },    // kVK_ANSI_H
        MacKeyCode { .keyCode = 0x05, .codepoint = U'g' },    // kVK_ANSI_G
        MacKeyCode { .keyCode = 0x06, .codepoint = U'z' },    // kVK_ANSI_Z
        MacKeyCode { .keyCode = 0x07, .codepoint = U'x' },    // kVK_ANSI_X
        MacKeyCode { .keyCode = 0x08, .codepoint = U'c' },    // kVK_ANSI_C
        MacKeyCode { .keyCode = 0x09, .codepoint = U'v' },    // kVK_ANSI_V
        MacKeyCode { .keyCode = 0x0B, .codepoint = U'b' },    // kVK_ANSI_B
        MacKeyCode { .keyCode = 0x0C, .codepoint = U'q' },    // kVK_ANSI_Q
        MacKeyCode { .keyCode = 0x0D, .codepoint = U'w' },    // kVK_ANSI_W
        MacKeyCode { .keyCode = 0x0E, .codepoint = U'e' },    // kVK_ANSI_E
        MacKeyCode { .keyCode = 0x0F, .codepoint = U'r' },    // kVK_ANSI_R
        MacKeyCode { .keyCode = 0x10, .codepoint = U'y' },    // kVK_ANSI_Y
        MacKeyCode { .keyCode = 0x11, .codepoint = U't' },    // kVK_ANSI_T
        MacKeyCode { .keyCode = 0x12, .codepoint = U'1' },    // kVK_ANSI_1
        MacKeyCode { .keyCode = 0x13, .codepoint = U'2' },    // kVK_ANSI_2
        MacKeyCode { .keyCode = 0x14, .codepoint = U'3' },    // kVK_ANSI_3
        MacKeyCode { .keyCode = 0x15, .codepoint = U'4' },    // kVK_ANSI_4
        MacKeyCode { .keyCode = 0x16, .codepoint = U'6' },    // kVK_ANSI_6
        MacKeyCode { .keyCode = 0x17, .codepoint = U'5' },    // kVK_ANSI_5
        MacKeyCode { .keyCode = 0x18, .codepoint = U'=' },    // kVK_ANSI_Equal
        MacKeyCode { .keyCode = 0x19, .codepoint = U'9' },    // kVK_ANSI_9
        MacKeyCode { .keyCode = 0x1A, .codepoint = U'7' },    // kVK_ANSI_7
        MacKeyCode { .keyCode = 0x1B, .codepoint = U'-' },    // kVK_ANSI_Minus
        MacKeyCode { .keyCode = 0x1C, .codepoint = U'8' },    // kVK_ANSI_8
        MacKeyCode { .keyCode = 0x1D, .codepoint = U'0' },    // kVK_ANSI_0
        MacKeyCode { .keyCode = 0x1E, .codepoint = U']' },    // kVK_ANSI_RightBracket
        MacKeyCode { .keyCode = 0x1F, .codepoint = U'o' },    // kVK_ANSI_O
        MacKeyCode { .keyCode = 0x20, .codepoint = U'u' },    // kVK_ANSI_U
        MacKeyCode { .keyCode = 0x21, .codepoint = U'[' },    // kVK_ANSI_LeftBracket
        MacKeyCode { .keyCode = 0x22, .codepoint = U'i' },    // kVK_ANSI_I
        MacKeyCode { .keyCode = 0x23, .codepoint = U'p' },    // kVK_ANSI_P
        MacKeyCode { .keyCode = 0x24, .codepoint = U'\r' },   // kVK_Return
        MacKeyCode { .keyCode = 0x25, .codepoint = U'l' },    // kVK_ANSI_L
        MacKeyCode { .keyCode = 0x26, .codepoint = U'j' },    // kVK_ANSI_J
        MacKeyCode { .keyCode = 0x27, .codepoint = U'\'' },   // kVK_ANSI_Quote
        MacKeyCode { .keyCode = 0x28, .codepoint = U'k' },    // kVK_ANSI_K
        MacKeyCode { .keyCode = 0x29, .codepoint = U';' },    // kVK_ANSI_Semicolon
        MacKeyCode { .keyCode = 0x2A, .codepoint = U'\\' },   // kVK_ANSI_Backslash
        MacKeyCode { .keyCode = 0x2B, .codepoint = U',' },    // kVK_ANSI_Comma
        MacKeyCode { .keyCode = 0x2C, .codepoint = U'/' },    // kVK_ANSI_Slash
        MacKeyCode { .keyCode = 0x2D, .codepoint = U'n' },    // kVK_ANSI_N
        MacKeyCode { .keyCode = 0x2E, .codepoint = U'm' },    // kVK_ANSI_M
        MacKeyCode { .keyCode = 0x2F, .codepoint = U'.' },    // kVK_ANSI_Period
        MacKeyCode { .keyCode = 0x30, .codepoint = U'\t' },   // kVK_Tab
        MacKeyCode { .keyCode = 0x31, .codepoint = U' ' },    // kVK_Space
        MacKeyCode { .keyCode = 0x32, .codepoint = U'`' },    // kVK_ANSI_Grave
        MacKeyCode { .keyCode = 0x35, .codepoint = U'\x1b' }, // kVK_Escape
    };

    static_assert(std::ranges::is_sorted(MacUsAnsiKeyCodes, {}, &MacKeyCode::keyCode),
                  "the table is bisected, so it must stay sorted by key code");

    [[nodiscard]] char32_t usLayoutKeyOfMacVirtualKey(uint32_t keyCode) noexcept
    {
        auto const i = std::ranges::lower_bound(MacUsAnsiKeyCodes, keyCode, {}, &MacKeyCode::keyCode);
        return i != MacUsAnsiKeyCodes.end() && i->keyCode == keyCode ? i->codepoint : char32_t { 0 };
    }

    /// The X11 keysym range that encodes a Unicode codepoint directly, as `0x01000000 | codepoint`.
    constexpr uint32_t X11UnicodeKeysymBase = 0x0100'0000;
    constexpr char32_t MaxCodepoint = 0x11'0000;

    /// A VK code exists only on Windows, so that is the only platform whose native key identifier
    /// may be reported as one. @see KeyIdentity::nativeVirtualKey.
    [[nodiscard]] constexpr uint32_t win32VirtualKeyOrNone(
        [[maybe_unused]] uint32_t nativeVirtualKey) noexcept
    {
#ifdef _WIN32
        return nativeVirtualKey;
#else
        return 0;
#endif
    }

    /// @see makePassthroughKeyboardLayout
    class PassthroughKeyboardLayout final: public KeyboardLayout
    {
      public:
        [[nodiscard]] char32_t unshiftedKeyOf(uint32_t nativeVirtualKey) const noexcept override
        {
            // Keysyms outside Latin-1 take the `0x01000000 | codepoint` form, which is not itself a
            // codepoint. Everything still out of range after that is a key we cannot name — a
            // function or media key — and 0 tells the caller to fall back to the character.
            auto const candidate = nativeVirtualKey >= X11UnicodeKeysymBase
                                       ? nativeVirtualKey - X11UnicodeKeysymBase
                                       : nativeVirtualKey;
            return candidate < MaxCodepoint ? static_cast<char32_t>(candidate) : char32_t { 0 };
        }

        [[nodiscard]] uint32_t win32VirtualKeyOf(uint32_t nativeVirtualKey) const noexcept override
        {
            return win32VirtualKeyOrNone(nativeVirtualKey);
        }
    };

    /// @see makeMacUsAnsiKeyboardLayout
    class MacUsAnsiKeyboardLayout final: public KeyboardLayout
    {
      public:
        [[nodiscard]] char32_t unshiftedKeyOf(uint32_t nativeVirtualKey) const noexcept override
        {
            return usLayoutKeyOfMacVirtualKey(nativeVirtualKey);
        }

        /// Never a VK code: this layout reads its input as a Carbon key code, which one is not. The
        /// answer does not depend on the host platform -- only on what the value being read is.
        [[nodiscard]] uint32_t win32VirtualKeyOf(uint32_t /*nativeVirtualKey*/) const noexcept override
        {
            return 0;
        }
    };

#ifdef __APPLE__
    struct CFReleaser
    {
        void operator()(CFTypeRef ref) const noexcept
        {
            if (ref)
                CFRelease(ref);
        }
    };

    using InputSourceHandle = std::unique_ptr<std::remove_pointer_t<TISInputSourceRef>, CFReleaser>;

    /// Asks the user's active keyboard layout what a key produces, because macOS is the one platform
    /// whose native key identifier is positional rather than codepoint-valued.
    ///
    /// Consulting the live layout rather than a fixed table is what makes Dvorak, German and every
    /// other non-US layout report the key the user actually pressed; the US ANSI table is only the
    /// fallback for when the layout cannot answer.
    class MacKeyboardLayout final: public KeyboardLayout
    {
      public:
        explicit MacKeyboardLayout(std::unique_ptr<KeyboardLayout> fallback):
            _fallback { std::move(fallback) }
        {
            // The layout is resolved once and reused, because this runs on every keystroke. macOS
            // posts this notification when the user switches input source, which is the only thing
            // that can invalidate the cache.
            CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(),
                                            this,
                                            &MacKeyboardLayout::onInputSourceChanged,
                                            kTISNotifySelectedKeyboardInputSourceChanged,
                                            nullptr,
                                            CFNotificationSuspensionBehaviorCoalesce);
        }

        ~MacKeyboardLayout() override
        {
            CFNotificationCenterRemoveEveryObserver(CFNotificationCenterGetDistributedCenter(), this);
        }

        [[nodiscard]] char32_t unshiftedKeyOf(uint32_t nativeVirtualKey) const noexcept override
        {
            if (auto const ch = translate(nativeVirtualKey); ch != 0)
                return ch;

            // A layout that cannot answer — a dead key, or one of the legacy 'KCHR' layouts that
            // carry no kTISPropertyUnicodeKeyLayoutData at all — still leaves the key's position
            // known, and its US-layout meaning is a better answer than none.
            return _fallback->unshiftedKeyOf(nativeVirtualKey);
        }

        [[nodiscard]] uint32_t win32VirtualKeyOf(uint32_t /*nativeVirtualKey*/) const noexcept override
        {
            return 0;
        }

      private:
        static void onInputSourceChanged(CFNotificationCenterRef /*center*/,
                                         void* observer,
                                         CFNotificationName /*name*/,
                                         void const* /*object*/,
                                         CFDictionaryRef /*userInfo*/) noexcept
        {
            auto& self = *static_cast<MacKeyboardLayout*>(observer);
            self._inputSource.reset();
            self._layoutData = nullptr;
            self._resolved = false;
        }

        /// The cache is plain, non-atomic mutable state because both sides of it are the main thread:
        /// distributed notifications are delivered on the main run loop, and key events reach us on
        /// Qt's GUI thread, which on macOS is that same thread.
        [[nodiscard]] UCKeyboardLayout const* layoutData() const noexcept
        {
            if (_resolved)
                return _layoutData;

            // Latched even when it fails: TISCopyCurrentKeyboardLayoutInputSource is the expensive
            // call here, and it can transiently return nothing while the user switches input source.
            // Retrying it per keystroke would spend the cost again for the same answer; the
            // notification above is what tells us the answer may have changed.
            _resolved = true;
            _inputSource.reset(TISCopyCurrentKeyboardLayoutInputSource());
            if (!_inputSource)
                return nullptr;

            // Not owned by us: TISGetInputSourceProperty follows the Get Rule, so the data lives as
            // long as the input source we are holding — which is what makes caching the pointer safe.
            auto const* data = static_cast<CFDataRef>(
                TISGetInputSourceProperty(_inputSource.get(), kTISPropertyUnicodeKeyLayoutData));
            _layoutData = data ? reinterpret_cast<UCKeyboardLayout const*>(CFDataGetBytePtr(data)) : nullptr;
            return _layoutData;
        }

        [[nodiscard]] char32_t translate(uint32_t nativeVirtualKey) const noexcept
        {
            auto const* layout = layoutData();
            if (!layout)
                return 0;

            auto deadKeyState = UInt32 { 0 };
            auto chars = std::array<char16_t, 4> {};
            auto length = UniCharCount { 0 };
            auto const status = UCKeyTranslate(layout,
                                               static_cast<UInt16>(nativeVirtualKey),
                                               kUCKeyActionDisplay,
                                               /*modifierKeyState=*/0,
                                               LMGetKbdType(),
                                               kUCKeyTranslateNoDeadKeysBit,
                                               &deadKeyState,
                                               chars.size(),
                                               &length,
                                               reinterpret_cast<UniChar*>(chars.data()));
            if (status != noErr || length == 0)
                return 0;

            // UTF-16 out, UTF-32 in. The zero-filled tail means a truncated surrogate pair decodes
            // to nullopt rather than reading past the end.
            auto const* cursor = chars.data();
            return unicode::decoder<char16_t> {}(cursor).value_or(char32_t { 0 });
        }

        std::unique_ptr<KeyboardLayout> _fallback {};
        mutable InputSourceHandle _inputSource {};
        mutable UCKeyboardLayout const* _layoutData = nullptr;
        mutable bool _resolved = false;
    };
#endif
} // namespace

std::unique_ptr<KeyboardLayout> makePassthroughKeyboardLayout()
{
    return std::make_unique<PassthroughKeyboardLayout>();
}

std::unique_ptr<KeyboardLayout> makeMacUsAnsiKeyboardLayout()
{
    return std::make_unique<MacUsAnsiKeyboardLayout>();
}

std::unique_ptr<KeyboardLayout> makePlatformKeyboardLayout()
{
#ifdef __APPLE__
    return std::make_unique<MacKeyboardLayout>(makeMacUsAnsiKeyboardLayout());
#else
    return makePassthroughKeyboardLayout();
#endif
}

} // namespace contour
