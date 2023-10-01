// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/logging.h>

#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <libunicode/convert.h>

using namespace std;

namespace vtbackend
{

namespace mappings
{
    struct KeyMapping
    {
        Key key;
        std::string_view mapping {};
    };

    // TODO: implement constexpr-binary-search by:
    // - adding operator<(KeyMapping a, KeyMapping b) { return a.key < b.key; }
    // - constexpr-evaluated sort()ed array returned in lambda-expr to be assigned to these globals here.
    // - make use of this property and let tryMap() do a std::binary_search()

#define ESC "\x1B"
#define CSI "\x1B["
#define SS3 "\x1BO"

    // the modifier parameter is going to be replaced via fmt::format()
    array<KeyMapping, 30> const functionKeysWithModifiers {
        // clang-format off
        // Note, that F1..F4 is using CSI too instead of ESC when used with modifier keys.
        // XXX: Maybe I am blind when reading ctlseqs.txt, but F1..F4 with "1;{}P".. seems not to
        // match what other terminal emulators send out with modifiers and I don't see how to match
        // xterm's behaviour along with getting for example vim working to bind to these.
        KeyMapping { Key::F1, ESC "O{}P" }, // "1;{}P"
        KeyMapping { Key::F2, ESC "O{}Q" }, // "1;{}Q"
        KeyMapping { Key::F3, ESC "O{}R" }, // "1;{}R"
        KeyMapping { Key::F4, ESC "O{}S" }, // "1;{}S"
        KeyMapping { Key::F5, CSI "15;{}~" },
        KeyMapping { Key::F6, CSI "17;{}~" },
        KeyMapping { Key::F7, CSI "18;{}~" },
        KeyMapping { Key::F8, CSI "19;{}~" },
        KeyMapping { Key::F9, CSI "20;{}~" },
        KeyMapping { Key::F10, CSI "21;{}~" },
        KeyMapping { Key::F11, CSI "23;{}~" },
        KeyMapping { Key::F12, CSI "24;{}~" },
        KeyMapping { Key::F13, CSI "25;{}~" },
        KeyMapping { Key::F14, CSI "26;{}~" },
        KeyMapping { Key::F15, CSI "28;{}~" },
        KeyMapping { Key::F16, CSI "29;{}~" },
        KeyMapping { Key::F17, CSI "31;{}~" },
        KeyMapping { Key::F18, CSI "32;{}~" },
        KeyMapping { Key::F19, CSI "33;{}~" },
        KeyMapping { Key::F20, CSI "34;{}~" },

        // cursor keys
        KeyMapping { Key::UpArrow, CSI "1;{}A" },
        KeyMapping { Key::DownArrow, CSI "1;{}B" },
        KeyMapping { Key::RightArrow, CSI "1;{}C" },
        KeyMapping { Key::LeftArrow, CSI "1;{}D" },

        // 6-key editing pad
        KeyMapping { Key::Insert, CSI "2;{}~" },
        KeyMapping { Key::Delete, CSI "3;{}~" },
        KeyMapping { Key::Home, CSI "1;{}H" },
        KeyMapping { Key::End, CSI "1;{}F" },
        KeyMapping { Key::PageUp, CSI "5;{}~" },
        KeyMapping { Key::PageDown, CSI "6;{}~" },
        // clang-format on
    };

    array<KeyMapping, 22> const standard {
        // clang-format off
        // cursor keys
        KeyMapping { Key::UpArrow, CSI "A" },
        KeyMapping { Key::DownArrow, CSI "B" },
        KeyMapping { Key::RightArrow, CSI "C" },
        KeyMapping { Key::LeftArrow, CSI "D" },

        // 6-key editing pad
        KeyMapping { Key::Insert, CSI "2~" },
        KeyMapping { Key::Delete, CSI "3~" },
        KeyMapping { Key::Home, CSI "H" },
        KeyMapping { Key::End, CSI "F" },
        KeyMapping { Key::PageUp, CSI "5~" },
        KeyMapping { Key::PageDown, CSI "6~" },

        // function keys
        KeyMapping { Key::F1, ESC "OP" },
        KeyMapping { Key::F2, ESC "OQ" },
        KeyMapping { Key::F3, ESC "OR" },
        KeyMapping { Key::F4, ESC "OS" },
        KeyMapping { Key::F5, CSI "15~" },
        KeyMapping { Key::F6, CSI "17~" },
        KeyMapping { Key::F7, CSI "18~" },
        KeyMapping { Key::F8, CSI "19~" },
        KeyMapping { Key::F9, CSI "20~" },
        KeyMapping { Key::F10, CSI "21~" },
        KeyMapping { Key::F11, CSI "23~" },
        KeyMapping { Key::F12, CSI "24~" },
        // clang-format on
    };

    /// (DECCKM) Cursor key mode: mappings in when cursor key application mode is set.
    array<KeyMapping, 6> const applicationCursorKeys {
        // clang-format off
        KeyMapping { Key::UpArrow, SS3 "A" },
        KeyMapping { Key::DownArrow, SS3 "B" },
        KeyMapping { Key::RightArrow, SS3 "C" },
        KeyMapping { Key::LeftArrow, SS3 "D" },
        KeyMapping { Key::Home, SS3 "H" },
        KeyMapping { Key::End, SS3 "F" },
        // clang-format on
    };

    array<KeyMapping, 21> const applicationKeypad
    {
        // clang-format off
        KeyMapping { Key::Numpad_NumLock, SS3 "P" },
        KeyMapping { Key::Numpad_Divide, SS3 "Q" },
        KeyMapping { Key::Numpad_Multiply, SS3 "Q" },
        KeyMapping { Key::Numpad_Subtract, SS3 "Q" },
        KeyMapping { Key::Numpad_CapsLock, SS3 "m" },
        KeyMapping { Key::Numpad_Add, SS3 "l" },
        KeyMapping { Key::Numpad_Decimal, SS3 "n" },
        KeyMapping { Key::Numpad_Enter, SS3 "M" },
        KeyMapping { Key::Numpad_Equal, SS3 "X" },
        KeyMapping { Key::Numpad_0, SS3 "p" },
        KeyMapping { Key::Numpad_1, SS3 "q" },
        KeyMapping { Key::Numpad_2, SS3 "r" },
        KeyMapping { Key::Numpad_3, SS3 "s" },
        KeyMapping { Key::Numpad_4, SS3 "t" },
        KeyMapping { Key::Numpad_5, SS3 "u" },
        KeyMapping { Key::Numpad_6, SS3 "v" },
        KeyMapping { Key::Numpad_7, SS3 "w" },
        KeyMapping { Key::Numpad_8, SS3 "x" },
        KeyMapping { Key::Numpad_9, SS3 "y" },
        KeyMapping { Key::PageUp, CSI "5~" },
        KeyMapping { Key::PageDown, CSI "6~" },
#if 0 // TODO
        KeyMapping{Key::Space,    SS3 " "}, // TODO
        KeyMapping{Key::Tab,      SS3 "I"},
        KeyMapping{Key::Enter,    SS3 "M"},
#endif
        // clang-format on
    };

#undef ESC
#undef CSI
#undef SS3

    constexpr bool operator==(KeyMapping const& km, Key key) noexcept
    {
        return km.key == key;
    }

    template <size_t N>
    optional<string_view> tryMap(array<KeyMapping, N> const& mappings, Key key) noexcept
    {
        for (KeyMapping const& km: mappings)
            if (km.key == key)
                return { km.mapping };

        return nullopt;
    }
} // namespace mappings

string to_string(Modifier modifier)
{
    string out;
    auto const append = [&](const char* s) {
        if (!out.empty())
            out += ",";
        out += s;
    };

    if (modifier.shift())
        append("Shift");
    if (modifier.alt())
        append("Alt");
    if (modifier.control())
        append("Control");
    if (modifier.meta())
        append("Meta");

    return out;
}

string to_string(Key key)
{
    switch (key)
    {
        case Key::F1: return "F1";
        case Key::F2: return "F2";
        case Key::F3: return "F3";
        case Key::F4: return "F4";
        case Key::F5: return "F5";
        case Key::F6: return "F6";
        case Key::F7: return "F7";
        case Key::F8: return "F8";
        case Key::F9: return "F9";
        case Key::F10: return "F10";
        case Key::F11: return "F11";
        case Key::F12: return "F12";
        case Key::F13: return "F13";
        case Key::F14: return "F14";
        case Key::F15: return "F15";
        case Key::F16: return "F16";
        case Key::F17: return "F17";
        case Key::F18: return "F18";
        case Key::F19: return "F19";
        case Key::F20: return "F20";
        case Key::DownArrow: return "DownArrow";
        case Key::LeftArrow: return "LeftArrow";
        case Key::RightArrow: return "RightArrow";
        case Key::UpArrow: return "UpArrow";
        case Key::Insert: return "Insert";
        case Key::Delete: return "Delete";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::PageUp: return "PageUp";
        case Key::PageDown: return "PageDown";
        case Key::Numpad_NumLock: return "Numpad_NumLock";
        case Key::Numpad_Divide: return "Numpad_Divide";
        case Key::Numpad_Multiply: return "Numpad_Multiply";
        case Key::Numpad_Subtract: return "Numpad_Subtract";
        case Key::Numpad_CapsLock: return "Numpad_CapsLock";
        case Key::Numpad_Add: return "Numpad_Add";
        case Key::Numpad_Decimal: return "Numpad_Decimal";
        case Key::Numpad_Enter: return "Numpad_Enter";
        case Key::Numpad_Equal: return "Numpad_Equal";
        case Key::Numpad_0: return "Numpad_0";
        case Key::Numpad_1: return "Numpad_1";
        case Key::Numpad_2: return "Numpad_2";
        case Key::Numpad_3: return "Numpad_3";
        case Key::Numpad_4: return "Numpad_4";
        case Key::Numpad_5: return "Numpad_5";
        case Key::Numpad_6: return "Numpad_6";
        case Key::Numpad_7: return "Numpad_7";
        case Key::Numpad_8: return "Numpad_8";
        case Key::Numpad_9: return "Numpad_9";
    }
    return "(unknown)";
}

string to_string(MouseButton button)
{
    switch (button)
    {
        case MouseButton::Left: return "Left"s;
        case MouseButton::Right: return "Right"s;
        case MouseButton::Middle: return "Middle"s;
        case MouseButton::Release: return "Release"s;
        case MouseButton::WheelUp: return "WheelUp"s;
        case MouseButton::WheelDown: return "WheelDown"s;
    }
    return ""; // should never be reached
}

void InputGenerator::reset()
{
    _cursorKeysMode = KeyMode::Normal;
    _numpadKeysMode = KeyMode::Normal;
    _bracketedPaste = false;
    _generateFocusEvents = false;
    _mouseProtocol = std::nullopt;
    _mouseTransport = MouseTransport::Default;
    _mouseWheelMode = MouseWheelMode::Default;

    // _pendingSequence = {};
    // _currentMousePosition = {0, 0}; // current mouse position
    // _currentlyPressedMouseButtons = {};
}

void InputGenerator::setCursorKeysMode(KeyMode mode)
{
    inputLog()("set cursor keys mode: {}", mode);
    _cursorKeysMode = mode;
}

void InputGenerator::setNumpadKeysMode(KeyMode mode)
{
    inputLog()("set numpad keys mode: {}", mode);
    _numpadKeysMode = mode;
}

void InputGenerator::setApplicationKeypadMode(bool enable)
{
    if (enable)
        _numpadKeysMode = KeyMode::Application;
    else
        _numpadKeysMode = KeyMode::Normal; // aka. Numeric

    inputLog()("set application keypad mode: {} -> {}", enable, _numpadKeysMode);
}

bool InputGenerator::generate(u32string const& characterEvent, Modifier modifier)
{
    for (char32_t const ch: characterEvent)
        generate(ch, modifier);
    return true;
}

bool InputGenerator::generate(char32_t characterEvent, Modifier modifier)
{
    char const chr = static_cast<char>(characterEvent);

    // See section "Alt and Meta Keys" in ctlseqs.txt from xterm.
    if (modifier.alt())
        // NB: There are other modes in xterm to send Alt+Key options or even send ESC on Meta key instead.
        append("\033");

    // Well accepted hack to distinguish between Backspace nad Ctrl+Backspace,
    // - Backspace is emitting 0x7f,
    // - Ctrl+Backspace is emitting 0x08
    if (characterEvent == 0x08)
    {
        if (!modifier.control())
            return append("\x7f");
        else
            return append("\x08");
    }

    if (modifier == Modifier::Shift && characterEvent == 0x09)
        return append("\033[Z"); // introduced by linux_console in 1995, adopted by xterm in 2002

    // raw C0 code
    if (modifier == Modifier::Control && characterEvent < 32)
        return append(static_cast<uint8_t>(characterEvent));

    if (modifier == Modifier::Control && characterEvent == L' ')
        return append('\x00');

    if (modifier == Modifier::Control && crispy::ascending('A', chr, 'Z'))
        return append(static_cast<char>(chr - 'A' + 1));

    if (modifier == Modifier::Control && characterEvent >= '[' && characterEvent <= '_')
        return append(static_cast<char>(chr - 'A' + 1)); // remaining C0 characters 0x1B .. 0x1F

    if (modifier.without(Modifier::Alt).none() || modifier == Modifier::Shift)
        return append(unicode::convert_to<char>(characterEvent));

    if (characterEvent < 0x7F)
        append(static_cast<char>(characterEvent));
    else
        append(unicode::convert_to<char>(characterEvent));

    inputLog()("Sending {} \"{}\".", modifier, crispy::escape(unicode::convert_to<char>(characterEvent)));
    return true;
}

bool InputGenerator::generate(Key key, Modifier modifier)
{
    auto const logged = [key, modifier](bool success) -> bool {
        if (success)
            inputLog()("Sending {} {}.", modifier, key);
        return success;
    };

    if (modifier)
    {
        if (auto mapping = tryMap(mappings::functionKeysWithModifiers, key); mapping)
            return logged(append(crispy::replace(*mapping, "{}"sv, makeVirtualTerminalParam(modifier))));
    }

    if (applicationCursorKeys())
        if (auto mapping = tryMap(mappings::applicationCursorKeys, key); mapping)
            return logged(append(*mapping));

    if (applicationKeypad())
        if (auto mapping = tryMap(mappings::applicationKeypad, key); mapping)
            return logged(append(*mapping));

    if (auto mapping = tryMap(mappings::standard, key); mapping)
        return logged(append(*mapping));

    return false;
}

void InputGenerator::generatePaste(std::string_view const& text)
{
    inputLog()("Sending paste of {} bytes.", text.size());

    if (text.empty())
        return;

    if (_bracketedPaste)
        append("\033[200~"sv);

    append(text);

    if (_bracketedPaste)
        append("\033[201~"sv);
}

inline bool InputGenerator::append(std::string_view sequence)
{
    _pendingSequence.insert(end(_pendingSequence), begin(sequence), end(sequence));
    return true;
}

inline bool InputGenerator::append(char asciiChar)
{
    _pendingSequence.push_back(asciiChar);
    return true;
}

inline bool InputGenerator::append(uint8_t byte)
{
    _pendingSequence.push_back(static_cast<char>(byte));
    return true;
}

inline bool InputGenerator::append(unsigned int asciiChar)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u", asciiChar);
    return append(string_view(buf, static_cast<size_t>(n)));
}

bool InputGenerator::generateFocusInEvent()
{
    if (generateFocusEvents())
    {
        append("\033[I");
        inputLog()("Sending focus-in event.");
        return true;
    }
    return false;
}

bool InputGenerator::generateFocusOutEvent()
{
    if (generateFocusEvents())
    {
        append("\033[O");
        inputLog()("Sending focus-out event.");
        return true;
    }
    return true;
}

bool InputGenerator::generateRaw(std::string_view const& raw)
{
    append(raw);
    return true;
}

// {{{ mouse handling
void InputGenerator::setMouseProtocol(MouseProtocol mouseProtocol, bool enabled)
{
    if (enabled)
    {
        _mouseWheelMode = MouseWheelMode::Default;
        _mouseProtocol = mouseProtocol;
    }
    else
        _mouseProtocol = std::nullopt;
}

void InputGenerator::setMouseTransport(MouseTransport mouseTransport)
{
    _mouseTransport = mouseTransport;
}

void InputGenerator::setMouseWheelMode(MouseWheelMode mode) noexcept
{
    _mouseWheelMode = mode;
}

namespace
{
    constexpr uint8_t modifierBits(Modifier modifier) noexcept
    {
        uint8_t mods = 0;
        if (modifier.shift())
            mods |= 4;
        if (modifier.meta())
            mods |= 8;
        if (modifier.control())
            mods |= 16;
        return mods;
    }

    constexpr uint8_t buttonNumber(MouseButton button) noexcept
    {
        switch (button)
        {
            case MouseButton::Left: return 0;
            case MouseButton::Middle: return 1;
            case MouseButton::Right: return 2;
            case MouseButton::Release: return 3;
            case MouseButton::WheelUp: return 4;
            case MouseButton::WheelDown: return 5;
        }
        return 0; // should never happen
    }

    constexpr bool isMouseWheel(MouseButton button) noexcept
    {
        return button == MouseButton::WheelUp || button == MouseButton::WheelDown;
    }

    constexpr uint8_t buttonX10(MouseButton button) noexcept
    {
        return isMouseWheel(button) ? uint8_t(buttonNumber(button) + 0x3c) : buttonNumber(button);
    }

    constexpr uint8_t buttonNormal(MouseButton button, InputGenerator::MouseEventType eventType) noexcept
    {
        return eventType == InputGenerator::MouseEventType::Release ? 3 : buttonX10(button);
    }
} // namespace

bool InputGenerator::generateMouse(MouseEventType eventType,
                                   Modifier modifier,
                                   MouseButton button,
                                   CellLocation pos,
                                   PixelCoordinate pixelPosition,
                                   bool uiHandled)
{
    if (!_mouseProtocol.has_value())
        return false;

    // std::cout << fmt::format("generateMouse({}/{}): button:{}, modifier:{}, at:{}, type:{}\n",
    //                          _mouseTransport, *_mouseProtocol,
    //                          button, modifier, pos, eventType);

    switch (*_mouseProtocol)
    {
        case MouseProtocol::X10: // Old X10 mouse protocol
            if (eventType == MouseEventType::Press)
                mouseTransport(
                    eventType, buttonX10(button), modifierBits(modifier), pos, pixelPosition, uiHandled);
            return true;
        case MouseProtocol::NormalTracking: // Normal tracking mode, that's X10 with mouse release events and
                                            // modifiers
            if (eventType == MouseEventType::Press || eventType == MouseEventType::Release)
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);
                mouseTransport(eventType, buttonValue, modifierBits(modifier), pos, pixelPosition, uiHandled);
            }
            return true;
        case MouseProtocol::ButtonTracking: // Button-event tracking protocol.
            // like normal event tracking, but with drag events
            if (eventType == MouseEventType::Press || eventType == MouseEventType::Drag
                || eventType == MouseEventType::Release)
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);

                uint8_t const draggableButton =
                    eventType == MouseEventType::Drag ? uint8_t(buttonValue + 0x20) : buttonValue;

                mouseTransport(
                    eventType, draggableButton, modifierBits(modifier), pos, pixelPosition, uiHandled);
                return true;
            }
            return false;
        case MouseProtocol::AnyEventTracking: // Like ButtonTracking but any motion events (not just dragging)
            // TODO: make sure we can receive mouse-move events even without mouse pressed.
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);

                uint8_t const draggableButton =
                    eventType == MouseEventType::Drag ? uint8_t(buttonValue + 0x20) : buttonValue;

                mouseTransport(
                    eventType, draggableButton, modifierBits(modifier), pos, pixelPosition, uiHandled);
            }
            return true;
        case MouseProtocol::HighlightTracking: // Highlight mouse tracking
            return false;                      // TODO: do we want to implement this?
    }

    return false;
}

bool InputGenerator::mouseTransport(MouseEventType eventType,
                                    uint8_t button,
                                    uint8_t modifier,
                                    CellLocation pos,
                                    PixelCoordinate pixelPosition,
                                    bool uiHandled)
{
    if (pos.line.value < 0 || pos.column.value < 0)
        // Negative coordinates are not supported. Avoid sending bad values.
        return true;

    switch (_mouseTransport)
    {
        case MouseTransport::Default: // mode: 9
            mouseTransportX10(button, modifier, pos);
            return true;
        case MouseTransport::Extended: // mode: 1005
            // TODO (like Default but with UTF-8 encoded coords)
            mouseTransportExtended(button, modifier, pos);
            return false;
        case MouseTransport::SGR: // mode: 1006
            return mouseTransportSGR(eventType, button, modifier, *pos.column + 1, *pos.line + 1, uiHandled);
        case MouseTransport::URXVT: // mode: 1015
            return mouseTransportURXVT(eventType, button, modifier, pos);
        case MouseTransport::SGRPixels: // mode: 1016
            return mouseTransportSGR(
                eventType, button, modifier, pixelPosition.x.value, pixelPosition.y.value, uiHandled);
    }

    return false;
}

bool InputGenerator::mouseTransportExtended(uint8_t button, uint8_t modifier, CellLocation pos)
{
    constexpr auto SkipCount = uint8_t { 0x20 }; // TODO std::numeric_limits<ControlCode>::max();
    constexpr auto MaxCoordValue = 2015;

    if (*pos.line < MaxCoordValue && *pos.column < MaxCoordValue)
    {
        auto const buttonValue = static_cast<uint8_t>(SkipCount + static_cast<uint8_t>(button | modifier));
        auto const line = static_cast<char32_t>(SkipCount + *pos.line + 1);
        auto const column = static_cast<char32_t>(SkipCount + *pos.column + 1);
        append("\033[M");
        append(buttonValue);
        append(unicode::convert_to<char>(column));
        append(unicode::convert_to<char>(line));
        return true;
    }
    else
        return false;
}

bool InputGenerator::mouseTransportX10(uint8_t button, uint8_t modifier, CellLocation pos)
{
    constexpr uint8_t SkipCount = 0x20; // TODO std::numeric_limits<ControlCode>::max();
    constexpr uint8_t MaxCoordValue = std::numeric_limits<uint8_t>::max() - SkipCount;

    if (*pos.line < MaxCoordValue && *pos.column < MaxCoordValue)
    {
        auto const buttonValue = static_cast<uint8_t>(SkipCount + static_cast<uint8_t>(button | modifier));
        auto const line = static_cast<uint8_t>(SkipCount + *pos.line + 1);
        auto const column = static_cast<uint8_t>(SkipCount + *pos.column + 1);
        append("\033[M");
        append(buttonValue);
        append(column);
        append(line);
        return true;
    }
    else
        return false;
}

bool InputGenerator::mouseTransportSGR(
    MouseEventType eventType, uint8_t button, uint8_t modifier, int x, int y, bool uiHandled)
{
    append("\033[<");
    append(static_cast<unsigned>(button | modifier));
    append(';');
    append(static_cast<unsigned>(x));
    append(';');
    append(static_cast<unsigned>(y));

    if (_passiveMouseTracking)
    {
        append(';');
        append(uiHandled ? '1' : '0');
    }

    append(eventType != MouseEventType::Release ? 'M' : 'm');

    return true;
}

bool InputGenerator::mouseTransportURXVT(MouseEventType eventType,
                                         uint8_t button,
                                         uint8_t modifier,
                                         CellLocation pos)
{
    if (eventType == MouseEventType::Press)
    {
        append("\033[");
        append(static_cast<unsigned>(button | modifier));
        append(';');
        append(static_cast<unsigned>(*pos.column + 1));
        append(';');
        append(static_cast<unsigned>(*pos.line + 1));
        append('M');
    }
    return true;
}

bool InputGenerator::generateMousePress(
    Modifier modifier, MouseButton button, CellLocation pos, PixelCoordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            inputLog()("Sending mouse press {} {} at {}.", button, modifier, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (!_mouseProtocol.has_value())
        return false;

    switch (mouseWheelMode())
    {
        case MouseWheelMode::NormalCursorKeys:
            if (_passiveMouseTracking)
                break;
            switch (button)
            {
                case MouseButton::WheelUp: return logged(append("\033[A"));
                case MouseButton::WheelDown: return logged(append("\033[B"));
                default: break;
            }
            break;
        case MouseWheelMode::ApplicationCursorKeys:
            if (_passiveMouseTracking)
                break;
            switch (button)
            {
                case MouseButton::WheelUp: return logged(append("\033OA"));
                case MouseButton::WheelDown: return logged(append("\033OB"));
                default: break;
            }
            break;
        case MouseWheelMode::Default: break;
    }

    if (!isMouseWheel(button))
        if (!_currentlyPressedMouseButtons.count(button))
            _currentlyPressedMouseButtons.insert(button);

    return logged(generateMouse(
        MouseEventType::Press, modifier, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool InputGenerator::generateMouseRelease(
    Modifier modifier, MouseButton button, CellLocation pos, PixelCoordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            inputLog()("Sending mouse release {} {} at {}.", button, modifier, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (auto i = _currentlyPressedMouseButtons.find(button); i != _currentlyPressedMouseButtons.end())
        _currentlyPressedMouseButtons.erase(i);

    return logged(generateMouse(
        MouseEventType::Release, modifier, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool InputGenerator::generateMouseMove(Modifier modifier,
                                       CellLocation pos,
                                       PixelCoordinate pixelPosition,
                                       bool uiHandled)
{
    if (pos == _currentMousePosition && _mouseTransport != MouseTransport::SGRPixels)
        // Only generate a mouse move event if the coordinate of interest(!) has actually changed.
        return false;

    auto const logged = [&](bool success) -> bool {
        if (success)
        {
            inputLog()("[{}:{}] Sending mouse move at {} ({}:{}).",
                       _mouseProtocol.value(),
                       _mouseTransport,
                       pos,
                       pixelPosition.x.value,
                       pixelPosition.y.value);
        }
        return success;
    };

    _currentMousePosition = pos;

    if (!_mouseProtocol.has_value())
        return false;

    bool const buttonsPressed = !_currentlyPressedMouseButtons.empty();

    bool const report = (_mouseProtocol.value() == MouseProtocol::ButtonTracking && buttonsPressed)
                        || _mouseProtocol.value() == MouseProtocol::AnyEventTracking;

    if (report)
        return logged(generateMouse(
            MouseEventType::Drag,
            modifier,
            buttonsPressed ? *_currentlyPressedMouseButtons.begin() // what if multiple are pressed?
                           : MouseButton::Release,
            pos,
            pixelPosition,
            uiHandled));

    return false;
}
// }}}

} // namespace vtbackend
