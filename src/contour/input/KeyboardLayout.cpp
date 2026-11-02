// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeysymTable.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#ifdef __APPLE__
    #include <type_traits>

    #include <Carbon/Carbon.h>
#endif

#ifdef _WIN32
    #include <windows.h>
#endif

namespace contour::input
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

    struct Win32KeyCode
    {
        uint32_t virtualKey;
        char32_t codepoint;
    };

    /// The Windows virtual-key codes that carry a character on a US keyboard, sorted by code so the lookup
    /// can bisect. Keys that produce no character -- modifiers, function keys, arrows -- are absent and
    /// resolve to 0.
    ///
    /// The letters and digits are their own codes (VK_A is 0x41, 'A'), but the OEM punctuation keys are
    /// positional codes with no arithmetic relationship to what they produce (VK_OEM_1 is 0xBA, ';'), so
    /// the table spells every row out. Adding a key is adding a row.
    constexpr auto Win32UsKeyCodes = std::array {
        Win32KeyCode { .virtualKey = 0x09, .codepoint = U'\t' },   // VK_TAB
        Win32KeyCode { .virtualKey = 0x0D, .codepoint = U'\r' },   // VK_RETURN
        Win32KeyCode { .virtualKey = 0x1B, .codepoint = U'\x1b' }, // VK_ESCAPE
        Win32KeyCode { .virtualKey = 0x20, .codepoint = U' ' },    // VK_SPACE
        Win32KeyCode { .virtualKey = 0x30, .codepoint = U'0' },
        Win32KeyCode { .virtualKey = 0x31, .codepoint = U'1' },
        Win32KeyCode { .virtualKey = 0x32, .codepoint = U'2' },
        Win32KeyCode { .virtualKey = 0x33, .codepoint = U'3' },
        Win32KeyCode { .virtualKey = 0x34, .codepoint = U'4' },
        Win32KeyCode { .virtualKey = 0x35, .codepoint = U'5' },
        Win32KeyCode { .virtualKey = 0x36, .codepoint = U'6' },
        Win32KeyCode { .virtualKey = 0x37, .codepoint = U'7' },
        Win32KeyCode { .virtualKey = 0x38, .codepoint = U'8' },
        Win32KeyCode { .virtualKey = 0x39, .codepoint = U'9' },
        Win32KeyCode { .virtualKey = 0x41, .codepoint = U'a' },
        Win32KeyCode { .virtualKey = 0x42, .codepoint = U'b' },
        Win32KeyCode { .virtualKey = 0x43, .codepoint = U'c' },
        Win32KeyCode { .virtualKey = 0x44, .codepoint = U'd' },
        Win32KeyCode { .virtualKey = 0x45, .codepoint = U'e' },
        Win32KeyCode { .virtualKey = 0x46, .codepoint = U'f' },
        Win32KeyCode { .virtualKey = 0x47, .codepoint = U'g' },
        Win32KeyCode { .virtualKey = 0x48, .codepoint = U'h' },
        Win32KeyCode { .virtualKey = 0x49, .codepoint = U'i' },
        Win32KeyCode { .virtualKey = 0x4A, .codepoint = U'j' },
        Win32KeyCode { .virtualKey = 0x4B, .codepoint = U'k' },
        Win32KeyCode { .virtualKey = 0x4C, .codepoint = U'l' },
        Win32KeyCode { .virtualKey = 0x4D, .codepoint = U'm' },
        Win32KeyCode { .virtualKey = 0x4E, .codepoint = U'n' },
        Win32KeyCode { .virtualKey = 0x4F, .codepoint = U'o' },
        Win32KeyCode { .virtualKey = 0x50, .codepoint = U'p' },
        Win32KeyCode { .virtualKey = 0x51, .codepoint = U'q' },
        Win32KeyCode { .virtualKey = 0x52, .codepoint = U'r' },
        Win32KeyCode { .virtualKey = 0x53, .codepoint = U's' },
        Win32KeyCode { .virtualKey = 0x54, .codepoint = U't' },
        Win32KeyCode { .virtualKey = 0x55, .codepoint = U'u' },
        Win32KeyCode { .virtualKey = 0x56, .codepoint = U'v' },
        Win32KeyCode { .virtualKey = 0x57, .codepoint = U'w' },
        Win32KeyCode { .virtualKey = 0x58, .codepoint = U'x' },
        Win32KeyCode { .virtualKey = 0x59, .codepoint = U'y' },
        Win32KeyCode { .virtualKey = 0x5A, .codepoint = U'z' },
        Win32KeyCode { .virtualKey = 0x60, .codepoint = U'0' }, // VK_NUMPAD0
        Win32KeyCode { .virtualKey = 0x61, .codepoint = U'1' },
        Win32KeyCode { .virtualKey = 0x62, .codepoint = U'2' },
        Win32KeyCode { .virtualKey = 0x63, .codepoint = U'3' },
        Win32KeyCode { .virtualKey = 0x64, .codepoint = U'4' },
        Win32KeyCode { .virtualKey = 0x65, .codepoint = U'5' },
        Win32KeyCode { .virtualKey = 0x66, .codepoint = U'6' },
        Win32KeyCode { .virtualKey = 0x67, .codepoint = U'7' },
        Win32KeyCode { .virtualKey = 0x68, .codepoint = U'8' },
        Win32KeyCode { .virtualKey = 0x69, .codepoint = U'9' },
        Win32KeyCode { .virtualKey = 0x6A, .codepoint = U'*' },  // VK_MULTIPLY
        Win32KeyCode { .virtualKey = 0x6B, .codepoint = U'+' },  // VK_ADD
        Win32KeyCode { .virtualKey = 0x6D, .codepoint = U'-' },  // VK_SUBTRACT
        Win32KeyCode { .virtualKey = 0x6E, .codepoint = U'.' },  // VK_DECIMAL
        Win32KeyCode { .virtualKey = 0x6F, .codepoint = U'/' },  // VK_DIVIDE
        Win32KeyCode { .virtualKey = 0xBA, .codepoint = U';' },  // VK_OEM_1
        Win32KeyCode { .virtualKey = 0xBB, .codepoint = U'=' },  // VK_OEM_PLUS
        Win32KeyCode { .virtualKey = 0xBC, .codepoint = U',' },  // VK_OEM_COMMA
        Win32KeyCode { .virtualKey = 0xBD, .codepoint = U'-' },  // VK_OEM_MINUS
        Win32KeyCode { .virtualKey = 0xBE, .codepoint = U'.' },  // VK_OEM_PERIOD
        Win32KeyCode { .virtualKey = 0xBF, .codepoint = U'/' },  // VK_OEM_2
        Win32KeyCode { .virtualKey = 0xC0, .codepoint = U'`' },  // VK_OEM_3
        Win32KeyCode { .virtualKey = 0xDB, .codepoint = U'[' },  // VK_OEM_4
        Win32KeyCode { .virtualKey = 0xDC, .codepoint = U'\\' }, // VK_OEM_5
        Win32KeyCode { .virtualKey = 0xDD, .codepoint = U']' },  // VK_OEM_6
        Win32KeyCode { .virtualKey = 0xDE, .codepoint = U'\'' }, // VK_OEM_7
    };

    static_assert(std::ranges::is_sorted(Win32UsKeyCodes, {}, &Win32KeyCode::virtualKey),
                  "the table is bisected, so it must stay sorted by virtual-key code");

    [[nodiscard]] char32_t usLayoutKeyOfWin32VirtualKey(uint32_t virtualKey) noexcept
    {
        auto const i = std::ranges::lower_bound(Win32UsKeyCodes, virtualKey, {}, &Win32KeyCode::virtualKey);
        return i != Win32UsKeyCodes.end() && i->virtualKey == virtualKey ? i->codepoint : char32_t { 0 };
    }

    /// @see makeFixedXkbKeymap
    class FixedXkbKeymap final: public XkbKeymap
    {
      public:
        explicit FixedXkbKeymap(std::span<FixedXkbKey const> keys): _keys { keys.begin(), keys.end() } {}

        [[nodiscard]] uint32_t levelZeroKeysymOf(uint32_t keycode, uint32_t group) const noexcept override
        {
            // The row of the highest group not beyond the one asked for: a key with fewer groups selects
            // its last.
            auto keysym = uint32_t { 0 };
            auto best = std::optional<uint32_t> {};
            for (auto const& key: _keys)
                if (key.keycode == keycode && key.group <= group && (!best || key.group >= *best))
                {
                    best = key.group;
                    keysym = key.keysym;
                }
            return keysym;
        }

      private:
        std::vector<FixedXkbKey> _keys;
    };

    /// @see makeXkbKeyboardLayout
    class XkbKeyboardLayout final: public KeyboardLayout
    {
      public:
        explicit XkbKeyboardLayout(std::unique_ptr<XkbKeymap> keymap): _keymap { std::move(keymap) } {}

        [[nodiscard]] char32_t unshiftedKeyOf(NativeKeyCode key) const noexcept override
        {
            // The group is the event's own, so a switch between layouts needs no notification.
            if (_keymap)
                if (auto const ch = keysymToCodepoint(_keymap->levelZeroKeysymOf(key.scanCode, key.group));
                    ch != 0)
                    return ch;

            // Everything still unnamed after the table is a key we cannot name -- a function or media
            // key -- and 0 tells the caller to fall back to the character.
            return keysymToCodepoint(key.virtualKey);
        }

        /// Only Windows has VK codes: a keysym must not reach ConPTY's win32-input-mode disguised as one.
        [[nodiscard]] uint32_t win32VirtualKeyOf(NativeKeyCode /*key*/) const noexcept override { return 0; }

      private:
        std::unique_ptr<XkbKeymap> _keymap;
    };

    /// @see makeMacUsAnsiKeyboardLayout
    class MacUsAnsiKeyboardLayout final: public KeyboardLayout
    {
      public:
        [[nodiscard]] char32_t unshiftedKeyOf(NativeKeyCode key) const noexcept override
        {
            return usLayoutKeyOfMacVirtualKey(key.virtualKey);
        }

        /// Never a VK code: this layout reads its input as a Carbon key code, which one is not. The
        /// answer does not depend on the host platform -- only on what the value being read is.
        [[nodiscard]] uint32_t win32VirtualKeyOf(NativeKeyCode /*key*/) const noexcept override { return 0; }
    };

    /// @see makeWin32UsKeyboardLayout
    class Win32UsKeyboardLayout final: public KeyboardLayout
    {
      public:
        [[nodiscard]] char32_t unshiftedKeyOf(NativeKeyCode key) const noexcept override
        {
            return usLayoutKeyOfWin32VirtualKey(key.virtualKey);
        }

        [[nodiscard]] uint32_t win32VirtualKeyOf(NativeKeyCode key) const noexcept override
        {
            return key.virtualKey;
        }
    };

#ifdef _WIN32
    /// Asks the user's active keyboard layout what a virtual key produces, because the VK codes of the OEM
    /// punctuation keys are positional: `VK_OEM_1` is ';' on a US keyboard and 'Ö' on a Swedish one.
    ///
    /// ToUnicodeEx runs with a zeroed key state, so the answer is the key's unmodified character, and with
    /// the "do not change the keyboard state" flag so that asking never disturbs a pending dead key. The
    /// US table is the fallback for a key the layout makes no character of.
    class Win32KeyboardLayout final: public KeyboardLayout
    {
      public:
        explicit Win32KeyboardLayout(std::unique_ptr<KeyboardLayout> fallback):
            _fallback { std::move(fallback) }
        {
        }

        [[nodiscard]] char32_t unshiftedKeyOf(NativeKeyCode key) const noexcept override
        {
            if (auto const ch = translate(key); ch != 0)
                return ch;
            return _fallback->unshiftedKeyOf(key);
        }

        [[nodiscard]] uint32_t win32VirtualKeyOf(NativeKeyCode key) const noexcept override
        {
            return key.virtualKey;
        }

      private:
        [[nodiscard]] static char32_t translate(NativeKeyCode key) noexcept
        {
            constexpr auto DoNotChangeKeyboardState = UINT { 1 << 2 };
            auto const keyState = std::array<BYTE, 256> {}; // all keys up: the unmodified level
            auto chars = std::array<wchar_t, 4> {};
            auto const count = ToUnicodeEx(key.virtualKey,
                                           key.scanCode,
                                           keyState.data(),
                                           chars.data(),
                                           static_cast<int>(chars.size()),
                                           DoNotChangeKeyboardState,
                                           GetKeyboardLayout(0));
            // Zero is no character, and a negative count is a dead key: the key stands for an accent
            // that composes with the next one, which is no unshifted character to report.
            if (count <= 0)
                return 0;

            // UTF-16 out, UTF-32 in. The zero-filled tail means a truncated surrogate pair decodes to
            // nullopt rather than reading past the end.
            auto const* cursor = reinterpret_cast<char16_t const*>(chars.data());
            auto const ch = unicode::decoder<char16_t> {}(cursor).value_or(char32_t { 0 });
            // A control character is a chord's product (Ctrl-state is zeroed, but Return and Tab still
            // are), not the key's character.
            return ch >= U' ' ? ch : char32_t { 0 };
        }

        std::unique_ptr<KeyboardLayout> _fallback {};
    };
#endif

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

        [[nodiscard]] char32_t unshiftedKeyOf(NativeKeyCode key) const noexcept override
        {
            if (auto const ch = translate(key.virtualKey); ch != 0)
                return ch;

            // A layout that cannot answer — a dead key, or one of the legacy 'KCHR' layouts that
            // carry no kTISPropertyUnicodeKeyLayoutData at all — still leaves the key's position
            // known, and its US-layout meaning is a better answer than none.
            return _fallback->unshiftedKeyOf(key);
        }

        [[nodiscard]] uint32_t win32VirtualKeyOf(NativeKeyCode /*key*/) const noexcept override { return 0; }

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

std::unique_ptr<XkbKeymap> makeFixedXkbKeymap(std::span<FixedXkbKey const> keys)
{
    return std::make_unique<FixedXkbKeymap>(keys);
}

std::unique_ptr<KeyboardLayout> makeXkbKeyboardLayout(std::unique_ptr<XkbKeymap> keymap)
{
    return std::make_unique<XkbKeyboardLayout>(std::move(keymap));
}

std::unique_ptr<KeyboardLayout> makeMacUsAnsiKeyboardLayout()
{
    return std::make_unique<MacUsAnsiKeyboardLayout>();
}

std::unique_ptr<KeyboardLayout> makeWin32UsKeyboardLayout()
{
    return std::make_unique<Win32UsKeyboardLayout>();
}

std::unique_ptr<KeyboardLayout> makePlatformKeyboardLayout()
{
#ifdef __APPLE__
    return std::make_unique<MacKeyboardLayout>(makeMacUsAnsiKeyboardLayout());
#elifdef _WIN32
    return std::make_unique<Win32KeyboardLayout>(makeWin32UsKeyboardLayout());
#else
    return makeXkbKeyboardLayout(makeLiveXkbKeymap());
#endif
}

} // namespace contour::input
