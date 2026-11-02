// SPDX-License-Identifier: Apache-2.0
//
// The characterization table: what today's Qt key path writes to the PTY for a given native key event,
// per platform kind. It pins current behaviour, deliberately including the behaviour that is wrong; a
// change to the key pipeline edits a row and says so in its commit.
//
// The Mac and Win32 rows cannot run on Linux -- their code paths are #ifdef'd -- so their expectations
// are derived by reading SessionInput.cpp and KeyMapping.cpp, and the macOS and Windows CI runners
// confirm them.
#pragma once

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/NativeKeyEvent.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace contour::test
{

/// How the application in the terminal asked for its keys to be encoded.
enum class KeyEncoding : uint8_t
{
    Legacy = 0,        //!< Nothing negotiated: VT100/xterm bytes.
    KittyDisambiguate, //!< Kitty keyboard protocol, flag 1 (what fish enables).
    KittyReportAll,    //!< Kitty keyboard protocol, flags 1 and 8: every key as an escape code.
    Win32InputMode,    //!< ConPTY's win32-input-mode (DECSET 9001).
};

/// Whether a key binding claims the event.
enum class ExpectedBinding : uint8_t
{
    None = 0, //!< No binding: the event reaches the encoder.
    Fires,    //!< A binding claims the event; nothing is written.
};

/// The terminal state a row runs in, for the bindings that are mode-dependent.
enum class KeyMode : uint8_t
{
    Default = 0,     //!< A fresh terminal: no selection, no vi mode.
    SelectionActive, //!< A selection exists (binding mode Select).
};

/// One row of the characterization table.
struct KeyCharacterizationRow
{
    std::string_view name; ///< Unique, and the name of the test section.
    config::KeyboardPlatformKind kind;
    config::OptionAsAlt optionAsAlt = config::OptionAsAlt::Off;
    KeyEncoding encoding = KeyEncoding::Legacy;
    input::NativeKeyEvent event;
    std::string_view expectedPty; ///< Exact bytes; empty means nothing written.
    ExpectedBinding binding = ExpectedBinding::None;
    std::string_view boundAction = {}; ///< Action name when binding == Fires.
    KeyMode mode = KeyMode::Default;   ///< The terminal state the row runs in.
};

namespace keybits
{
    // Qt::KeyboardModifier values.
    inline constexpr uint32_t Shift = 0x02000000;
    inline constexpr uint32_t Ctrl = 0x04000000;
    inline constexpr uint32_t Alt = 0x08000000;
    inline constexpr uint32_t Meta = 0x10000000;
    inline constexpr uint32_t Keypad = 0x20000000;

    // Qt::Key values used by the table (spelled out so the header needs no Qt include).
    inline constexpr int KeyEscape = 0x01000000;
    inline constexpr int KeyTab = 0x01000001;
    inline constexpr int KeyBacktab = 0x01000002;
    inline constexpr int KeyBackspace = 0x01000003;
    inline constexpr int KeyReturn = 0x01000004;
    inline constexpr int KeyEnter = 0x01000005;
    inline constexpr int KeyEnd = 0x01000011;
    inline constexpr int KeyLeft = 0x01000012;
    inline constexpr int KeyUp = 0x01000013;
    inline constexpr int KeyF1 = 0x01000030;
    inline constexpr int KeyF12 = 0x0100003b;
    inline constexpr int KeySpace = 0x20;

    // macOS NSEvent flag bits, as Qt reports them in nativeModifiers().
    inline constexpr uint32_t MacCapsLock = 0x00010000;
    inline constexpr uint32_t MacOption = 0x00080000;
    inline constexpr uint32_t MacLeftOption = MacOption | 0x20;
    inline constexpr uint32_t MacRightOption = MacOption | 0x40;
    // XCB modifier mask bits, as Qt reports them on X11 and Wayland.
    inline constexpr uint32_t XcbCapsLock = 0x02;
    inline constexpr uint32_t XcbNumLock = 0x10;
} // namespace keybits

/// The table. Rows of the three platform kinds, in one list; a test runs those of its own kind.
/// @return The rows, in a stable order, valid for the life of the program.
[[nodiscard]] inline std::span<KeyCharacterizationRow const> keyCharacterizationRows()
{
    using config::KeyboardPlatformKind;
    using config::OptionAsAlt;
    using input::KeyEventOrigin;
    using input::NativeKeyEvent;
    using vtbackend::KeyboardEventType;
    using namespace keybits;
    using namespace std::string_view_literals;

    static auto const rows = [] {
        auto table = std::vector<KeyCharacterizationRow> {};

        auto const add = [&](std::string_view name,
                             KeyboardPlatformKind kind,
                             NativeKeyEvent event,
                             std::string_view expected,
                             KeyEncoding encoding = KeyEncoding::Legacy,
                             OptionAsAlt optionAsAlt = OptionAsAlt::Off) {
            table.push_back(KeyCharacterizationRow { .name = name,
                                                     .kind = kind,
                                                     .optionAsAlt = optionAsAlt,
                                                     .encoding = encoding,
                                                     .event = std::move(event),
                                                     .expectedPty = expected });
        };
        auto const addBound = [&](std::string_view name,
                                  KeyboardPlatformKind kind,
                                  NativeKeyEvent event,
                                  std::string_view action,
                                  KeyMode mode = KeyMode::Default) {
            table.push_back(KeyCharacterizationRow { .name = name,
                                                     .kind = kind,
                                                     .event = std::move(event),
                                                     .expectedPty = {},
                                                     .binding = ExpectedBinding::Fires,
                                                     .boundAction = action,
                                                     .mode = mode });
        };

        // {{{ Xkb: X11 and Wayland. nativeVirtualKey is a keysym; the passthrough layout reads it.
        constexpr auto Xkb = KeyboardPlatformKind::Xkb;
        add("xkb: printable ASCII", Xkb, { .qtKey = 'X', .text = U"x" }, "x");
        add("xkb: Shift+letter", Xkb, { .qtKey = 'A', .text = U"A", .qtModifiers = Shift }, "A");
        // Only under kitty report-all does the lock state show: it is carried in the encoded modifiers.
        add("xkb: CapsLock letter under kitty report-all",
            Xkb,
            { .qtKey = 'A', .text = U"A", .nativeModifiers = XcbCapsLock, .nativeVirtualKey = 0x41 },
            "\033[97;65u",
            KeyEncoding::KittyReportAll);
        // Plain Ctrl+C is the copy chord the defaults bind while a selection exists.
        addBound("xkb: Ctrl+C with a selection is a binding",
                 Xkb,
                 { .qtKey = 'C', .text = U"\x03", .qtModifiers = Ctrl },
                 "CopySelection",
                 KeyMode::SelectionActive);
        add("xkb: Ctrl+letter", Xkb, { .qtKey = 'C', .text = U"\x03", .qtModifiers = Ctrl }, "\003");
        addBound("xkb: Ctrl+Shift+letter is a binding",
                 Xkb,
                 { .qtKey = 'P', .text = U"\x10", .qtModifiers = Ctrl | Shift },
                 "OpenCommandPalette");
        add("xkb: Alt+letter", Xkb, { .qtKey = 'B', .text = U"b", .qtModifiers = Alt }, "\033b");
        add("xkb: keypad digit with NumLock",
            Xkb,
            { .qtKey = '5', .text = U"5", .qtModifiers = Keypad, .nativeModifiers = XcbNumLock },
            "5");
        add("xkb: keypad End without NumLock", Xkb, { .qtKey = KeyEnd, .qtModifiers = Keypad }, "\033[F");
        add("xkb: keypad Enter", Xkb, { .qtKey = KeyEnter, .text = U"\r", .qtModifiers = Keypad }, "\r");
        add("xkb: Backtab", Xkb, { .qtKey = KeyBacktab, .qtModifiers = Shift }, "\033[Z");
        add("xkb: F1", Xkb, { .qtKey = KeyF1 }, "\033OP");
        add("xkb: F12", Xkb, { .qtKey = KeyF12 }, "\033[24~");
        addBound("xkb: Shift+Up is a binding", Xkb, { .qtKey = KeyUp, .qtModifiers = Shift }, "ScrollOneUp");
        add("xkb: Ctrl+Left", Xkb, { .qtKey = KeyLeft, .qtModifiers = Ctrl }, "\033[1;5D");
        add("xkb: Ctrl+Up", Xkb, { .qtKey = KeyUp, .qtModifiers = Ctrl }, "\033[1;5A");
        add("xkb: Alt+Left", Xkb, { .qtKey = KeyLeft, .qtModifiers = Alt }, "\033[1;3D");
        add("xkb: Escape", Xkb, { .qtKey = KeyEscape }, "\033");
        add("xkb: Backspace", Xkb, { .qtKey = KeyBackspace }, "\177");
        add("xkb: Ctrl+Space", Xkb, { .qtKey = KeySpace, .qtModifiers = Ctrl }, "\0"sv);
        add("xkb: Enter", Xkb, { .qtKey = KeyReturn, .text = U"\r" }, "\r");
        addBound("xkb: Alt+Enter is a binding",
                 Xkb,
                 { .qtKey = KeyReturn, .text = U"\r", .qtModifiers = Alt },
                 "ToggleFullscreen");
        add("xkb: Shift+Enter", Xkb, { .qtKey = KeyReturn, .text = U"\r", .qtModifiers = Shift }, "\r");
        add("xkb: Ctrl+U under kitty disambiguate",
            Xkb,
            { .qtKey = 'U', .qtModifiers = Ctrl, .nativeVirtualKey = 0x75 },
            "\033[117;5u",
            KeyEncoding::KittyDisambiguate);
        add("xkb: Ctrl+U under kitty report-all",
            Xkb,
            { .qtKey = 'U', .qtModifiers = Ctrl, .nativeVirtualKey = 0x75 },
            "\033[117;5u",
            KeyEncoding::KittyReportAll);
        add("xkb: Cyrillic letter under kitty disambiguate",
            Xkb,
            { .qtKey = 0x0416, .text = U"ж", .nativeVirtualKey = 0x06d6 },
            "\xd0\xb6",
            KeyEncoding::KittyDisambiguate);
        add("xkb: Cyrillic letter under kitty report-all",
            Xkb,
            { .qtKey = 0x0416, .text = U"ж", .nativeVirtualKey = 0x06d6 },
            "\033[1750u",
            KeyEncoding::KittyReportAll);
        // }}}

        // {{{ Win32. nativeVirtualKey is a VK code; nativeModifiers is replaced by the live lock state, so
        // it is left zero here.
        constexpr auto Win32 = KeyboardPlatformKind::Win32;
        add("win32: printable ASCII", Win32, { .qtKey = 'X', .text = U"x", .nativeVirtualKey = 0x58 }, "x");
        add("win32: Shift+letter",
            Win32,
            { .qtKey = 'A', .text = U"A", .qtModifiers = Shift, .nativeVirtualKey = 0x41 },
            "A");
        // No CapsLock row: the Windows key path replaces nativeModifiers with the live toggle state, so a
        // table row cannot state it.
        // Plain Ctrl+C is the copy chord the defaults bind while a selection exists.
        addBound("win32: Ctrl+C with a selection is a binding",
                 Win32,
                 { .qtKey = 'C', .text = U"\x03", .qtModifiers = Ctrl, .nativeVirtualKey = 0x43 },
                 "CopySelection",
                 KeyMode::SelectionActive);
        add("win32: Ctrl+letter",
            Win32,
            { .qtKey = 'C', .text = U"\x03", .qtModifiers = Ctrl, .nativeVirtualKey = 0x43 },
            "\003");
        addBound("win32: Ctrl+Shift+letter is a binding",
                 Win32,
                 { .qtKey = 'P', .text = U"\x10", .qtModifiers = Ctrl | Shift, .nativeVirtualKey = 0x50 },
                 "OpenCommandPalette");
        add("win32: Alt+letter",
            Win32,
            { .qtKey = 'B', .text = U"b", .qtModifiers = Alt, .nativeVirtualKey = 0x42 },
            "\033b");
        add("win32: AltGr character",
            Win32,
            { .qtKey = '[', .text = U"[", .qtModifiers = Ctrl | Alt, .nativeVirtualKey = 0x38 },
            "[");
        add("win32: Ctrl+Alt chord",
            Win32,
            { .qtKey = 'A', .qtModifiers = Ctrl | Alt, .nativeVirtualKey = 0x41 },
            "\033\001");
        add("win32: keypad digit with NumLock",
            Win32,
            { .qtKey = '5', .text = U"5", .qtModifiers = Keypad, .nativeVirtualKey = 0x65 },
            "5");
        add("win32: keypad End without NumLock",
            Win32,
            { .qtKey = KeyEnd, .qtModifiers = Keypad, .nativeVirtualKey = 0x23 },
            "\033[F");
        add("win32: keypad Enter",
            Win32,
            { .qtKey = KeyEnter, .text = U"\r", .qtModifiers = Keypad, .nativeVirtualKey = 0x0d },
            "\r");
        add("win32: Backtab", Win32, { .qtKey = KeyBacktab, .qtModifiers = Shift }, "\033[Z");
        add("win32: F1", Win32, { .qtKey = KeyF1 }, "\033OP");
        add("win32: F12", Win32, { .qtKey = KeyF12 }, "\033[24~");
        addBound(
            "win32: Shift+Up is a binding", Win32, { .qtKey = KeyUp, .qtModifiers = Shift }, "ScrollOneUp");
        add("win32: Ctrl+Left", Win32, { .qtKey = KeyLeft, .qtModifiers = Ctrl }, "\033[1;5D");
        add("win32: Ctrl+Up", Win32, { .qtKey = KeyUp, .qtModifiers = Ctrl }, "\033[1;5A");
        add("win32: Alt+Left", Win32, { .qtKey = KeyLeft, .qtModifiers = Alt }, "\033[1;3D");
        add("win32: Escape", Win32, { .qtKey = KeyEscape }, "\033");
        add("win32: Backspace", Win32, { .qtKey = KeyBackspace }, "\177");
        add("win32: Ctrl+Space", Win32, { .qtKey = KeySpace, .qtModifiers = Ctrl }, "\0"sv);
        add("win32: Enter", Win32, { .qtKey = KeyReturn, .text = U"\r" }, "\r");
        addBound("win32: Alt+Enter is a binding",
                 Win32,
                 { .qtKey = KeyReturn, .text = U"\r", .qtModifiers = Alt },
                 "ToggleFullscreen");
        add("win32: Shift+Enter", Win32, { .qtKey = KeyReturn, .text = U"\r", .qtModifiers = Shift }, "\r");
        add("win32: Ctrl+U under kitty disambiguate",
            Win32,
            { .qtKey = 'U', .qtModifiers = Ctrl, .nativeVirtualKey = 0x55 },
            "\033[117;5u",
            KeyEncoding::KittyDisambiguate);
        add("win32: Ctrl+U under kitty report-all",
            Win32,
            { .qtKey = 'U', .qtModifiers = Ctrl, .nativeVirtualKey = 0x55 },
            "\033[117;5u",
            KeyEncoding::KittyReportAll);
        add("win32: Cyrillic letter under kitty disambiguate",
            Win32,
            { .qtKey = 0x0416, .text = U"ж", .nativeVirtualKey = 0xba },
            "\xd0\xb6",
            KeyEncoding::KittyDisambiguate);
        add("win32: Cyrillic letter under kitty report-all",
            Win32,
            { .qtKey = 0x0416, .text = U"ж", .nativeVirtualKey = 0xba },
            "\033[186u",
            KeyEncoding::KittyReportAll);
        add("win32: AltGr [ under win32-input-mode (#2127)",
            Win32,
            { .qtKey = '[', .text = U"[", .qtModifiers = Ctrl | Alt, .nativeVirtualKey = 0x38 },
            "\033[56;0;91;1;0;1_",
            KeyEncoding::Win32InputMode);
        // }}}

        // {{{ Mac. nativeVirtualKey is a positional Carbon key code; nativeModifiers holds NSEvent flags.
        constexpr auto Mac = KeyboardPlatformKind::Mac;
        add("mac: printable ASCII", Mac, { .qtKey = 'X', .text = U"x", .nativeVirtualKey = 0x07 }, "x");
        add("mac: Shift+letter",
            Mac,
            { .qtKey = 'A', .text = U"A", .qtModifiers = Shift, .nativeVirtualKey = 0x00 },
            "A");
        add("mac: CapsLock letter under kitty report-all",
            Mac,
            { .qtKey = 'A', .text = U"A", .nativeModifiers = MacCapsLock, .nativeVirtualKey = 0x00 },
            "\033[97;65u",
            KeyEncoding::KittyReportAll);
        // Plain Ctrl+C is the copy chord the defaults bind while a selection exists.
        addBound("mac: Ctrl+C with a selection is a binding",
                 Mac,
                 { .qtKey = 'C', .text = U"\x03", .qtModifiers = Ctrl, .nativeVirtualKey = 0x08 },
                 "CopySelection",
                 KeyMode::SelectionActive);
        add("mac: Ctrl+letter",
            Mac,
            { .qtKey = 'C', .text = U"\x03", .qtModifiers = Ctrl, .nativeVirtualKey = 0x08 },
            "\003");
        addBound("mac: Ctrl+Shift+letter is a binding",
                 Mac,
                 { .qtKey = 'P', .text = U"\x10", .qtModifiers = Ctrl | Shift, .nativeVirtualKey = 0x23 },
                 "OpenCommandPalette");
        add("mac: keypad digit",
            Mac,
            { .qtKey = '5', .text = U"5", .qtModifiers = Keypad, .nativeVirtualKey = 0x57 },
            "5");
        add("mac: keypad Enter",
            Mac,
            { .qtKey = KeyEnter, .text = U"\r", .qtModifiers = Keypad, .nativeVirtualKey = 0x4c },
            "\r");
        add("mac: Backtab", Mac, { .qtKey = KeyBacktab, .qtModifiers = Shift }, "\033[Z");
        add("mac: F1", Mac, { .qtKey = KeyF1 }, "\033OP");
        add("mac: F12", Mac, { .qtKey = KeyF12 }, "\033[24~");
        addBound("mac: Shift+Up is a binding", Mac, { .qtKey = KeyUp, .qtModifiers = Shift }, "ScrollOneUp");
        add("mac: Ctrl+Left", Mac, { .qtKey = KeyLeft, .qtModifiers = Ctrl }, "\033[1;5D");
        add("mac: Ctrl+Up", Mac, { .qtKey = KeyUp, .qtModifiers = Ctrl }, "\033[1;5A");
        add("mac: Alt+Left", Mac, { .qtKey = KeyLeft, .qtModifiers = Alt }, "\033[1;3D");
        add("mac: Escape", Mac, { .qtKey = KeyEscape }, "\033");
        add("mac: Backspace", Mac, { .qtKey = KeyBackspace }, "\177");
        add("mac: Ctrl+Space",
            Mac,
            { .qtKey = KeySpace, .qtModifiers = Ctrl, .nativeVirtualKey = 0x31 },
            "\0"sv);
        add("mac: Enter", Mac, { .qtKey = KeyReturn, .text = U"\r" }, "\r");
        add("mac: Shift+Enter", Mac, { .qtKey = KeyReturn, .text = U"\r", .qtModifiers = Shift }, "\r");
        add("mac: Ctrl+U under kitty disambiguate",
            Mac,
            { .qtKey = 'U', .qtModifiers = Ctrl, .nativeVirtualKey = 0x20 },
            "\033[117;5u",
            KeyEncoding::KittyDisambiguate);
        add("mac: Ctrl+U under kitty report-all",
            Mac,
            { .qtKey = 'U', .qtModifiers = Ctrl, .nativeVirtualKey = 0x20 },
            "\033[117;5u",
            KeyEncoding::KittyReportAll);
        add("mac: Cyrillic letter under kitty disambiguate",
            Mac,
            { .qtKey = 0x0416, .text = U"ж", .nativeVirtualKey = 0x29 },
            "\xd0\xb6",
            KeyEncoding::KittyDisambiguate);
        add("mac: Cyrillic letter under kitty report-all",
            Mac,
            { .qtKey = 0x0416, .text = U"ж", .nativeVirtualKey = 0x29 },
            "\033[59u",
            KeyEncoding::KittyReportAll);

        // Option+E, with Option held on the left or the right, under each option_as_alt setting. A side the
        // setting does not name composes the accented character instead.
        struct OptionCase
        {
            std::string_view name;
            OptionAsAlt setting;
            uint32_t option;
            std::string_view expected;
        };
        constexpr auto Alted = "\033e"sv;
        constexpr auto Composed = "\xc3\xa9"sv;
        for (auto const& c: { OptionCase { "mac: Option+e, option_as_alt off, left Option",
                                           OptionAsAlt::Off,
                                           MacLeftOption,
                                           Composed },
                              OptionCase { "mac: Option+e, option_as_alt off, right Option",
                                           OptionAsAlt::Off,
                                           MacRightOption,
                                           Composed },
                              OptionCase { "mac: Option+e, option_as_alt left, left Option",
                                           OptionAsAlt::Left,
                                           MacLeftOption,
                                           Alted },
                              OptionCase { "mac: Option+e, option_as_alt left, right Option",
                                           OptionAsAlt::Left,
                                           MacRightOption,
                                           Composed },
                              OptionCase { "mac: Option+e, option_as_alt right, left Option",
                                           OptionAsAlt::Right,
                                           MacLeftOption,
                                           Composed },
                              OptionCase { "mac: Option+e, option_as_alt right, right Option",
                                           OptionAsAlt::Right,
                                           MacRightOption,
                                           Alted },
                              OptionCase { "mac: Option+e, option_as_alt both, left Option",
                                           OptionAsAlt::Both,
                                           MacLeftOption,
                                           Alted },
                              OptionCase { "mac: Option+e, option_as_alt both, right Option",
                                           OptionAsAlt::Both,
                                           MacRightOption,
                                           Alted } })
        {
            add(c.name,
                Mac,
                { .qtKey = 'E', .text = U"é", .qtModifiers = Alt, .nativeModifiers = c.option },
                c.expected,
                KeyEncoding::Legacy,
                c.setting);
        }

        // Option+Enter: a named key, so the option_as_alt branch never sees it.
        table.push_back(KeyCharacterizationRow { .name = "mac: Option+Enter is a binding",
                                                 .kind = Mac,
                                                 .event = { .qtKey = KeyReturn,
                                                            .text = U"\r",
                                                            .qtModifiers = Alt,
                                                            .nativeModifiers = MacLeftOption },
                                                 .expectedPty = {},
                                                 .binding = ExpectedBinding::Fires,
                                                 .boundAction = "ToggleFullscreen" });

        // Command is Qt::MetaModifier. Command+C is unbound -- Copy is the menu's -- and today the chord
        // reaches the application as a bare "c" (the text route strips Alt, and the encoder drops Super).
        add("mac: Command+c, unbound",
            Mac,
            { .qtKey = 'C', .text = U"c", .qtModifiers = Meta, .nativeVirtualKey = 0x08 },
            "c");
        addBound("mac: Command+T is a binding",
                 Mac,
                 { .qtKey = 'T', .text = U"t", .qtModifiers = Meta, .nativeVirtualKey = 0x11 },
                 "CreateNewTab");
        // }}}

        return table;
    }();
    return rows;
}

} // namespace contour::test
