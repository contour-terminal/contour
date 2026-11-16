// SPDX-License-Identifier: Apache-2.0
#include <core/tui/VtParser.hpp>

#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace core::tui
{

namespace
{

    // ========================================================================
    // Win32 Virtual Key Code constants (local definitions for cross-platform compilation)
    // ========================================================================
    namespace vk
    {
        constexpr auto Back = 0x08;
        constexpr auto Tab = 0x09;
        constexpr auto Return = 0x0D;
        constexpr auto Shift = 0x10;
        constexpr auto Control = 0x11;
        constexpr auto Menu = 0x12; // Alt key
        constexpr auto Escape = 0x1B;
        constexpr auto Space = 0x20;
        constexpr auto Prior = 0x21; // Page Up
        constexpr auto Next = 0x22;  // Page Down
        constexpr auto End = 0x23;
        constexpr auto Home = 0x24;
        constexpr auto Left = 0x25;
        constexpr auto Up = 0x26;
        constexpr auto Right = 0x27;
        constexpr auto Down = 0x28;
        constexpr auto Insert = 0x2D;
        constexpr auto Delete = 0x2E;
        constexpr auto LWin = 0x5B;
        constexpr auto RWin = 0x5C;
        constexpr auto Numpad0 = 0x60;
        constexpr auto Numpad1 = 0x61;
        constexpr auto Numpad2 = 0x62;
        constexpr auto Numpad3 = 0x63;
        constexpr auto Numpad4 = 0x64;
        constexpr auto Numpad5 = 0x65;
        constexpr auto Numpad6 = 0x66;
        constexpr auto Numpad7 = 0x67;
        constexpr auto Numpad8 = 0x68;
        constexpr auto Numpad9 = 0x69;
        constexpr auto Multiply = 0x6A;
        constexpr auto Add = 0x6B;
        constexpr auto Separator = 0x6C;
        constexpr auto Subtract = 0x6D;
        constexpr auto Decimal = 0x6E;
        constexpr auto Divide = 0x6F;
        constexpr auto F1 = 0x70;
        constexpr auto F2 = 0x71;
        constexpr auto F3 = 0x72;
        constexpr auto F4 = 0x73;
        constexpr auto F5 = 0x74;
        constexpr auto F6 = 0x75;
        constexpr auto F7 = 0x76;
        constexpr auto F8 = 0x77;
        constexpr auto F9 = 0x78;
        constexpr auto F10 = 0x79;
        constexpr auto F11 = 0x7A;
        constexpr auto F12 = 0x7B;
        constexpr auto LShift = 0xA0;
        constexpr auto RMenu = 0xA5; // Right Alt — end of modifier VK range
    } // namespace vk

    // Win32 dwControlKeyState bit masks
    namespace cks
    {
        constexpr auto RightAlt = 0x0001;
        constexpr auto LeftAlt = 0x0002;
        constexpr auto RightCtrl = 0x0004;
        constexpr auto LeftCtrl = 0x0008;
        constexpr auto Shift = 0x0010;
        constexpr auto NumLock = 0x0020;
        [[maybe_unused]] constexpr auto ScrollLock = 0x0040;
        constexpr auto CapsLock = 0x0080;
        [[maybe_unused]] constexpr auto EnhancedKey = 0x0100;
        constexpr auto RightWin = 0x0200; // Windows Terminal custom extension
        constexpr auto LeftWin = 0x0400;  // Windows Terminal custom extension
    } // namespace cks

    /// @brief Retrieves a parameter at the given index with a default fallback.
    ///
    /// Win32 input mode parameters are all optional with defaults (Vk=0, Sc=0, Uc=0,
    /// Kd=0, Cs=0, Rc=1), matching the Windows Terminal reference implementation.
    /// @param params The parsed parameter list.
    /// @param index The parameter index.
    /// @param defaultValue The value to return if index is out of range.
    /// @return The parameter value at index, or defaultValue if absent.
    constexpr auto paramAt(std::vector<int> const& params, std::size_t index, int defaultValue = 0) -> int
    {
        return index < params.size() ? params[index] : defaultValue;
    }

    /// @brief Detects whether the AltGr key is active in a Win32 dwControlKeyState.
    ///
    /// On European keyboards, AltGr sends RightAlt|LeftCtrl simultaneously. The Windows
    /// Terminal reference only checks for RightAlt AND LeftCtrl being set, without excluding
    /// other modifier bits.
    /// @param controlKeyState The dwControlKeyState field from a Win32 key event.
    /// @return True if the AltGr combination is detected.
    constexpr auto isAltGrKeyState(int controlKeyState) -> bool
    {
        return (controlKeyState & cks::RightAlt) && (controlKeyState & cks::LeftCtrl);
    }

    /// @brief Decodes Win32 dwControlKeyState bits into a Modifier bitmask.
    /// @param controlKeyState The dwControlKeyState field from a Win32 key event.
    /// @return The corresponding Modifier bitmask.
    constexpr auto decodeWin32Modifiers(int controlKeyState) -> Modifier
    {
        auto mods = Modifier::None;
        if (controlKeyState & cks::Shift)
            mods |= Modifier::Shift;

        // AltGr on European keyboards sends RightAlt|LeftCtrl simultaneously.
        // Suppress both Alt and Ctrl so that AltGr-typed characters carry no spurious modifier flags.
        if (!isAltGrKeyState(controlKeyState))
        {
            if (controlKeyState & (cks::LeftAlt | cks::RightAlt))
                mods |= Modifier::Alt;
            if (controlKeyState & (cks::LeftCtrl | cks::RightCtrl))
                mods |= Modifier::Ctrl;
        }

        if (controlKeyState & (cks::LeftWin | cks::RightWin))
            mods |= Modifier::Super;
        if (controlKeyState & cks::CapsLock)
            mods |= Modifier::CapsLock;
        if (controlKeyState & cks::NumLock)
            mods |= Modifier::NumLock;
        return mods;
    }

    /// @brief Checks whether a Win32 virtual key code is a modifier-only key (Shift, Ctrl, Alt, Win).
    /// @param vkCode The virtual key code.
    /// @return True if the key is a bare modifier press.
    constexpr auto isWin32ModifierOnlyVk(int vkCode) -> bool
    {
        return vkCode == vk::Shift || vkCode == vk::Control || vkCode == vk::Menu || vkCode == vk::LWin
               || vkCode == vk::RWin || (vkCode >= vk::LShift && vkCode <= vk::RMenu);
    }

    /// @brief Maps a Win32 virtual key code to a KeyCode for special (non-printable) keys.
    /// @param vkCode The virtual key code.
    /// @return The corresponding KeyCode, or nullopt for printable/unrecognized keys.
    constexpr auto mapWin32VkToKeyCode(int vkCode) -> std::optional<KeyCode>
    {
        switch (vkCode)
        {
                // clang-format off
            case vk::Back:   return KeyCode::Backspace;
            case vk::Tab:    return KeyCode::Tab;
            case vk::Return: return KeyCode::Enter;
            case vk::Escape: return KeyCode::Escape;
            case vk::Prior:  return KeyCode::PageUp;
            case vk::Next:   return KeyCode::PageDown;
            case vk::End:    return KeyCode::End;
            case vk::Home:   return KeyCode::Home;
            case vk::Left:   return KeyCode::Left;
            case vk::Up:     return KeyCode::Up;
            case vk::Right:  return KeyCode::Right;
            case vk::Down:   return KeyCode::Down;
            case vk::Insert: return KeyCode::Insert;
            case vk::Delete: return KeyCode::Delete;
            case vk::F1:     return KeyCode::F1;
            case vk::F2:     return KeyCode::F2;
            case vk::F3:     return KeyCode::F3;
            case vk::F4:     return KeyCode::F4;
            case vk::F5:     return KeyCode::F5;
            case vk::F6:     return KeyCode::F6;
            case vk::F7:     return KeyCode::F7;
            case vk::F8:     return KeyCode::F8;
            case vk::F9:     return KeyCode::F9;
            case vk::F10:      return KeyCode::F10;
            case vk::F11:      return KeyCode::F11;
            case vk::F12:      return KeyCode::F12;
            //
            case vk::Numpad0:  return KeyCode::Kp0;
            case vk::Numpad1:  return KeyCode::Kp1;
            case vk::Numpad2:  return KeyCode::Kp2;
            case vk::Numpad3:  return KeyCode::Kp3;
            case vk::Numpad4:  return KeyCode::Kp4;
            case vk::Numpad5:  return KeyCode::Kp5;
            case vk::Numpad6:  return KeyCode::Kp6;
            case vk::Numpad7:  return KeyCode::Kp7;
            case vk::Numpad8:  return KeyCode::Kp8;
            case vk::Numpad9:  return KeyCode::Kp9;
            case vk::Multiply: return KeyCode::KpMultiply;
            case vk::Add:      return KeyCode::KpAdd;
            case vk::Separator: return KeyCode::KpSeparator;
            case vk::Subtract: return KeyCode::KpSubtract;
            case vk::Decimal:  return KeyCode::KpDecimal;
            case vk::Divide:   return KeyCode::KpDivide;
            default:           return std::nullopt;
                // clang-format on
        }
    }

    /// @brief Parses semicolon-separated integer parameters from a CSI parameter string.
    ///
    /// Colons within a parameter field are treated as subparameter separators per the
    /// Kitty keyboard protocol. Only the value before the first colon is extracted.
    /// @param buf The parameter string (e.g. "0;10;20" or "51:35;2").
    /// @return Vector of parsed integer values (first subparameter of each field).
    auto parseCsiParams(std::string_view buf) -> std::vector<int>
    {
        auto result = std::vector<int> {};
        for (auto const part: buf | std::views::split(';'))
        {
            auto sv = std::string_view(part.begin(), part.end());
            auto value = 0;
            // from_chars stops at the first non-digit (e.g., ':'), so "51:35" → 51
            if (auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value); ec == std::errc {})
                result.push_back(value);
            else
                result.push_back(0);
        }
        return result;
    }

    /// @brief Parsed components of the CSI u key field (colon-separated subparameters).
    struct KeyFieldInfo
    {
        int key = 0;           ///< Base/un-shifted key codepoint.
        int shiftedKey = 0;    ///< Shifted key codepoint (0 if absent).
        int baseLayoutKey = 0; ///< PC-101 base layout key codepoint (0 if absent).
    };

    /// @brief Parses the key field from a CSI u sequence.
    ///
    /// Format: key[:shifted_key[:base_layout_key]]
    /// @param field The raw first semicolon-separated parameter string.
    /// @return Parsed key field components.
    auto parseKeyField(std::string_view field) -> KeyFieldInfo
    {
        auto info = KeyFieldInfo {};
        auto const* it = field.data();
        auto const* const end = field.data() + field.size();

        auto parseNext = [&](int& target) {
            if (it == end)
                return;
            auto const [ptr, ec] = std::from_chars(it, end, target);
            if (ec == std::errc {})
                it = ptr;
            // Skip the colon separator if present
            if (it != end && *it == ':')
                ++it;
        };

        parseNext(info.key);
        parseNext(info.shiftedKey);
        parseNext(info.baseLayoutKey);
        return info;
    }

    /// @brief Decodes CSI modifier parameter to Modifier bitmask.
    ///
    /// CSI modifiers use the convention: encoded = 1 + modifier_bits
    /// where modifier_bits follows Kitty keyboard protocol:
    ///   bit 0: Shift, bit 1: Alt, bit 2: Ctrl, bit 3: Super,
    ///   bit 4: Hyper, bit 5: Meta, bit 6: CapsLock, bit 7: NumLock
    /// @param param The raw modifier parameter from CSI sequence.
    /// @return The decoded Modifier bitmask.
    constexpr auto decodeModifiers(int param) -> Modifier
    {
        if (param <= 1)
            return Modifier::None;
        auto const bits = param - 1;
        auto mods = Modifier::None;
        if (bits & 1)
            mods |= Modifier::Shift;
        if (bits & 2)
            mods |= Modifier::Alt;
        if (bits & 4)
            mods |= Modifier::Ctrl;
        if (bits & 8)
            mods |= Modifier::Super;
        // Hyper (bit 4) and Meta (bit 5) are rarely used, skipped
        if (bits & 64)
            mods |= Modifier::CapsLock;
        if (bits & 128)
            mods |= Modifier::NumLock;
        return mods;
    }

    /// @brief Maps a CSI final byte with optional params to a KeyCode for cursor/function keys.
    /// @param finalByte The CSI final byte.
    /// @param params The parsed parameter list.
    /// @return The corresponding KeyEvent, or nullopt if not recognized.
    auto mapCsiKey(char finalByte, std::vector<int> const& params) -> std::optional<KeyEvent>
    {
        auto const modifier = (params.size() >= 2) ? decodeModifiers(params[1]) : Modifier::None;

        switch (finalByte)
        {
            case 'A': return KeyEvent { .key = KeyCode::Up, .modifiers = modifier };
            case 'B': return KeyEvent { .key = KeyCode::Down, .modifiers = modifier };
            case 'C': return KeyEvent { .key = KeyCode::Right, .modifiers = modifier };
            case 'D': return KeyEvent { .key = KeyCode::Left, .modifiers = modifier };
            case 'H': return KeyEvent { .key = KeyCode::Home, .modifiers = modifier };
            case 'F': return KeyEvent { .key = KeyCode::End, .modifiers = modifier };
            case '~': {
                if (params.empty())
                    return std::nullopt;
                switch (params[0])
                {
                    case 1: return KeyEvent { .key = KeyCode::Home, .modifiers = modifier };
                    case 2: return KeyEvent { .key = KeyCode::Insert, .modifiers = modifier };
                    case 3: return KeyEvent { .key = KeyCode::Delete, .modifiers = modifier };
                    case 4: return KeyEvent { .key = KeyCode::End, .modifiers = modifier };
                    case 5: return KeyEvent { .key = KeyCode::PageUp, .modifiers = modifier };
                    case 6: return KeyEvent { .key = KeyCode::PageDown, .modifiers = modifier };
                    case 11: return KeyEvent { .key = KeyCode::F1, .modifiers = modifier };
                    case 12: return KeyEvent { .key = KeyCode::F2, .modifiers = modifier };
                    case 13: return KeyEvent { .key = KeyCode::F3, .modifiers = modifier };
                    case 14: return KeyEvent { .key = KeyCode::F4, .modifiers = modifier };
                    case 15: return KeyEvent { .key = KeyCode::F5, .modifiers = modifier };
                    case 17: return KeyEvent { .key = KeyCode::F6, .modifiers = modifier };
                    case 18: return KeyEvent { .key = KeyCode::F7, .modifiers = modifier };
                    case 19: return KeyEvent { .key = KeyCode::F8, .modifiers = modifier };
                    case 20: return KeyEvent { .key = KeyCode::F9, .modifiers = modifier };
                    case 21: return KeyEvent { .key = KeyCode::F10, .modifiers = modifier };
                    case 23: return KeyEvent { .key = KeyCode::F11, .modifiers = modifier };
                    case 24: return KeyEvent { .key = KeyCode::F12, .modifiers = modifier };
                    default: return std::nullopt;
                }
            }
            default: return std::nullopt;
        }
    }

    /// @brief Resolves the text codepoint for a numpad key based on modifier state.
    ///
    /// Operators always produce text. Digits/decimal produce text only when NumLock is active.
    /// @param key The numpad key code.
    /// @param mods Active modifier keys.
    /// @param unicodeHint Optional platform-provided character (Win32 unicodeChar); 0 to use default mapping.
    constexpr auto resolveNumpadCodepoint(KeyCode key, Modifier mods, int unicodeHint = 0) -> char32_t
    {
        auto const ncp = numpadCodepoint(key);
        if (ncp == 0)
            return 0;
        if (!isNumpadOperator(key) && !hasModifier(mods, Modifier::NumLock))
            return 0;
        return (unicodeHint >= 32) ? static_cast<char32_t>(unicodeHint) : ncp;
    }

    /// @brief Removes Win32 input mode sequences (CSI Vk;Sc;Uc;Kd;Cs;Rc _) from a paste buffer,
    /// replacing key-down events with their Unicode character and stripping key-up events entirely.
    ///
    /// Windows Terminal may emit Win32 input mode sequences for newlines and other keys
    /// inside bracketed paste regions when both modes are active simultaneously.
    /// @param buf The raw paste buffer content.
    /// @return Cleaned string with Win32 sequences replaced by their actual characters.
    auto sanitizeWin32PasteSequences(std::string_view buf) -> std::string
    {
        auto result = std::string {};
        result.reserve(buf.size());

        std::size_t i = 0;
        while (i < buf.size())
        {
            // Look for ESC [ ... _ pattern (Win32 input mode sequence)
            if (buf[i] == '\x1b' && i + 1 < buf.size() && buf[i + 1] == '[')
            {
                // Find the end of the CSI sequence: scan for the final byte '_'
                auto const seqStart = i;
                auto j = i + 2; // skip ESC [

                // Collect parameter bytes (digits and semicolons)
                while (j < buf.size() && ((buf[j] >= '0' && buf[j] <= '9') || buf[j] == ';'))
                    ++j;

                // Check if this CSI sequence ends with '_' (Win32 input mode finalizer)
                if (j < buf.size() && buf[j] == '_')
                {
                    // Parse the parameters: Vk;Sc;Uc;Kd;Cs;Rc
                    auto const paramStr = buf.substr(seqStart + 2, j - (seqStart + 2));
                    auto const params = parseCsiParams(paramStr);

                    auto const unicodeChar = paramAt(params, 2);
                    auto const keyDown = paramAt(params, 3);

                    if (keyDown != 0 && unicodeChar > 0)
                    {
                        // Key-down with a character: emit the actual character.
                        // CR (13) → '\n' for proper line separation.
                        if (unicodeChar == '\r')
                            result += '\n';
                        else
                            result += static_cast<char>(unicodeChar);
                    }
                    // Key-up events (keyDown==0) and modifier-only keys (unicodeChar==0)
                    // are silently dropped.

                    i = j + 1; // advance past the '_' finalizer
                    continue;
                }

                // Not a Win32 input mode sequence — fall through to copy the ESC literally
            }

            result += buf[i];
            ++i;
        }

        return result;
    }

    /// @brief The bracketed paste terminator.
    constexpr auto PasteEnd = std::string_view { "\033[201~" };

    /// @brief The ST (String Terminator) that ends a DCS string: ESC \.
    constexpr auto StringTerminator = std::string_view { "\033\\" };

    // How large each buffer may grow: the cap on what the sequence CARRIES, plus room for the
    // terminator that is on its way in. Both terminators land in the same buffer as the payload,
    // one byte at a time, and are taken off again once they are recognised -- so without the
    // reservation a payload of exactly its cap would be cut mid-terminator, and the documented
    // cap would be a few bytes larger than the longest sequence that actually works.
    constexpr auto MaxPasteBuffer = VtParser::MaxPasteLength + PasteEnd.size();
    constexpr auto MaxDcsBuffer = VtParser::MaxDcsLength + StringTerminator.size();
    constexpr auto MaxOscBuffer = VtParser::MaxOscLength + StringTerminator.size();

    /// @brief BEL, the terminator xterm also accepts for an OSC in place of ST.
    constexpr auto Bell = std::uint8_t { 0x07 };

} // namespace

auto VtParser::feed(std::string_view data) -> std::vector<InputEvent>
{
    auto events = std::vector<InputEvent> {};
    for (auto const ch: data)
    {
        auto const byte = static_cast<std::uint8_t>(ch);
        switch (_state)
        {
            case State::Ground: processGround(byte, events); break;
            case State::Escape: processEscape(byte, events); break;
            case State::CsiEntry:
            case State::CsiParam: processCsi(byte, events); break;
            case State::Ss3: processSs3(byte, events); break;
            case State::PasteBody: processPaste(byte, events); break;
            case State::Utf8Sequence: processUtf8(byte, events); break;
            case State::DcsEntry: processDcsEntry(byte, events); break;
            case State::DcsBody: processDcsBody(byte, events); break;
            case State::OscBody: processOscBody(byte, events); break;
        }
    }
    return events;
}

auto VtParser::timeout() -> std::vector<InputEvent>
{
    auto events = std::vector<InputEvent> {};
    if (_state == State::Escape)
    {
        // Bare ESC — resolve as Escape key
        events.emplace_back(KeyEvent { .key = KeyCode::Escape });
        _state = State::Ground;
    }
    return events;
}

auto VtParser::abandonIfOverlong(std::string& buffer, std::size_t cap) -> bool
{
    if (buffer.size() <= cap)
        return false;

    buffer.clear();
    buffer.shrink_to_fit(); // The point of the cap is the memory, so give it back.
    _state = State::Ground;
    return true;
}

void VtParser::processGround(std::uint8_t byte, std::vector<InputEvent>& events)
{
    if (byte == 0x1B)
    {
        _state = State::Escape;
        return;
    }

    // Control characters
    if (byte < 0x20)
    {
        switch (byte)
        {
            case '\r':
            case '\n': events.emplace_back(KeyEvent { .key = KeyCode::Enter }); break;
            case '\t': events.emplace_back(KeyEvent { .key = KeyCode::Tab }); break;
            case 0x7F:
            case 0x08: events.emplace_back(KeyEvent { .key = KeyCode::Backspace }); break;
            default: {
                // Ctrl+letter: byte = letter - 'a' + 1
                auto const cp = static_cast<char32_t>(byte + 'a' - 1);
                events.emplace_back(KeyEvent {
                    .key = keyCodeFromCodepoint(cp), .modifiers = Modifier::Ctrl, .codepoint = cp });
                break;
            }
        }
        return;
    }

    if (byte == 0x7F)
    {
        events.emplace_back(KeyEvent { .key = KeyCode::Backspace });
        return;
    }

    // UTF-8 multi-byte lead byte
    if ((byte & 0x80) != 0)
    {
        _utf8Buf.clear();
        _utf8Buf += static_cast<char>(byte);
        if ((byte & 0xE0) == 0xC0)
        {
            _utf8Remaining = 1;
        }
        else if ((byte & 0xF0) == 0xE0)
        {
            _utf8Remaining = 2;
        }
        else if ((byte & 0xF8) == 0xF0)
        {
            _utf8Remaining = 3;
        }
        else
        {
            // Invalid lead byte, ignore
            return;
        }
        _state = State::Utf8Sequence;
        return;
    }

    // ASCII printable
    emitCodepoint(static_cast<char32_t>(byte), events);
}

void VtParser::processEscape(std::uint8_t byte, std::vector<InputEvent>& events)
{
    if (byte == '[')
    {
        _paramBuf.clear();
        _state = State::CsiEntry;
        return;
    }

    if (byte == 'O')
    {
        _state = State::Ss3;
        return;
    }

    // DCS introducer: ESC P
    if (byte == 'P')
    {
        _dcsBuf.clear();
        _state = State::DcsEntry;
        return;
    }

    // OSC introducer: ESC ], an OSC reply only where the caller asked for those.
    if (byte == ']' && _options.osc == OscRecognition::Response)
    {
        _oscBuf.clear();
        _state = State::OscBody;
        return;
    }

    // Alt+Enter: ESC followed by CR or LF
    if (byte == '\r' || byte == '\n')
    {
        events.emplace_back(KeyEvent { .key = KeyCode::Enter, .modifiers = Modifier::Alt });
        _state = State::Ground;
        return;
    }

    // Alt+Backspace: ESC DEL, the spelling xterm, VTE, iTerm and Alacritty send, and ESC BS for
    // Alt+Ctrl+H. The fallback below would read either as a bare Escape -- which a modal takes as
    // cancel -- followed by a plain Backspace (core-cpp#21).
    if (byte == 0x7F || byte == 0x08)
    {
        events.emplace_back(KeyEvent { .key = KeyCode::Backspace, .modifiers = Modifier::Alt });
        _state = State::Ground;
        return;
    }

    // Alt+character: ESC followed by printable
    if (byte >= 0x20 && byte < 0x7F)
    {
        auto const cp = static_cast<char32_t>(byte);
        events.emplace_back(
            KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = Modifier::Alt, .codepoint = cp });
        _state = State::Ground;
        return;
    }

    // ESC ESC or ESC + control: emit ESC then reprocess
    events.emplace_back(KeyEvent { .key = KeyCode::Escape });
    _state = State::Ground;
    processGround(byte, events);
}

void VtParser::processCsi(std::uint8_t byte, std::vector<InputEvent>& events)
{
    // Parameter bytes: digits, semicolons, colons (subparameters), and private markers
    if ((byte >= '0' && byte <= '9') || byte == ';' || byte == ':' || byte == '<' || byte == '>'
        || byte == '?')
    {
        _paramBuf += static_cast<char>(byte);
        _state = State::CsiParam;
        std::ignore = abandonIfOverlong(_paramBuf, MaxCsiParamLength);
        return;
    }

    // Final byte in range [0x40, 0x7E]
    if (byte >= 0x40 && byte <= 0x7E)
    {
        dispatchCsi(static_cast<char>(byte), events);
        // Only reset to Ground if dispatchCsi didn't change state (e.g. to PasteBody)
        if (_state == State::CsiEntry || _state == State::CsiParam)
            _state = State::Ground;
        return;
    }

    // Intermediate bytes (0x20-0x2F) — accumulate
    if (byte >= 0x20 && byte <= 0x2F)
    {
        _paramBuf += static_cast<char>(byte);
        std::ignore = abandonIfOverlong(_paramBuf, MaxCsiParamLength);
        return;
    }

    // Unexpected byte — abort sequence
    _state = State::Ground;
}

void VtParser::processSs3(std::uint8_t byte, std::vector<InputEvent>& events)
{
    _state = State::Ground;
    switch (static_cast<char>(byte))
    {
        case 'A': events.emplace_back(KeyEvent { .key = KeyCode::Up }); break;
        case 'B': events.emplace_back(KeyEvent { .key = KeyCode::Down }); break;
        case 'C': events.emplace_back(KeyEvent { .key = KeyCode::Right }); break;
        case 'D': events.emplace_back(KeyEvent { .key = KeyCode::Left }); break;
        case 'H': events.emplace_back(KeyEvent { .key = KeyCode::Home }); break;
        case 'F': events.emplace_back(KeyEvent { .key = KeyCode::End }); break;
        case 'P': events.emplace_back(KeyEvent { .key = KeyCode::F1 }); break;
        case 'Q': events.emplace_back(KeyEvent { .key = KeyCode::F2 }); break;
        case 'R': events.emplace_back(KeyEvent { .key = KeyCode::F3 }); break;
        case 'S': events.emplace_back(KeyEvent { .key = KeyCode::F4 }); break;
        default: break;
    }
}

void VtParser::processPaste(std::uint8_t byte, std::vector<InputEvent>& events)
{
    _pasteBuf += static_cast<char>(byte);

    // Check for bracketed paste end: ESC[201~
    if (_pasteBuf.ends_with(PasteEnd))
    {
        // Remove the end sequence from the paste buffer
        _pasteBuf.resize(_pasteBuf.size() - PasteEnd.size());
        // Strip Win32 input mode sequences that Windows Terminal may embed
        // inside bracketed paste regions (e.g. Enter key as CSI 13;28;13;1;0;1 _).
        auto cleaned = sanitizeWin32PasteSequences(_pasteBuf);
        events.emplace_back(PasteEvent { .text = std::move(cleaned) });
        _pasteBuf.clear();
        _pasteBuf.shrink_to_fit();
        _state = State::Ground;
        return;
    }

    // Past the cap the paste ends here. Unlike the other two buffers this one holds the user's
    // own text, so it is emitted rather than dropped; what follows is read as ordinary input,
    // which is what happens anyway to a paste whose terminator is lost. The bound is the buffer's,
    // not the paste's: a paste of exactly MaxPasteLength bytes is still ended by its terminator
    // above, whose bytes pass through this buffer first.
    if (_pasteBuf.size() > MaxPasteBuffer)
    {
        auto cleaned = sanitizeWin32PasteSequences(_pasteBuf);
        events.emplace_back(PasteEvent { .text = std::move(cleaned) });
        _pasteBuf.clear();
        _pasteBuf.shrink_to_fit();
        _state = State::Ground;
    }
}

void VtParser::processUtf8(std::uint8_t byte, std::vector<InputEvent>& events)
{
    if ((byte & 0xC0) != 0x80)
    {
        // Invalid continuation byte — discard sequence, reprocess
        _utf8Buf.clear();
        _state = State::Ground;
        processGround(byte, events);
        return;
    }

    _utf8Buf += static_cast<char>(byte);
    --_utf8Remaining;

    if (_utf8Remaining == 0)
    {
        emitUtf8(events);
        _utf8Buf.clear();
        _state = State::Ground;
    }
}

void VtParser::dispatchCsi(char finalByte, std::vector<InputEvent>& events)
{
    // Check for focus events (DECSET 1004): CSI I = focus-in, CSI O = focus-out.
    // These have no parameters (empty _paramBuf).
    if (_paramBuf.empty() && finalByte == 'I')
    {
        events.emplace_back(FocusEvent { .focused = true });
        return;
    }
    if (_paramBuf.empty() && finalByte == 'O')
    {
        events.emplace_back(FocusEvent { .focused = false });
        return;
    }

    // Check for SGR mouse: ESC[<button;x;y[;uiHandled]M or ESC[<button;x;y[;uiHandled]m
    // The optional 4th parameter (uiHandled) is part of passive mouse tracking (DEC mode 2029).
    if (!_paramBuf.empty() && _paramBuf[0] == '<' && (finalByte == 'M' || finalByte == 'm'))
    {
        auto const params = parseCsiParams(_paramBuf.substr(1));
        if (params.size() >= 3)
        {
            auto const rawButton = params[0];
            auto const x = params[1];
            auto const y = params[2];
            auto const uiHandled = (params.size() >= 4) && (params[3] != 0);
            auto const isRelease = (finalByte == 'm');

            auto mods = Modifier::None;
            if (rawButton & 4)
                mods |= Modifier::Shift;
            if (rawButton & 8)
                mods |= Modifier::Alt;
            if (rawButton & 16)
                mods |= Modifier::Ctrl;

            auto const buttonBits = rawButton & 0x43; // button encoding bits

            if (buttonBits == 64)
            {
                events.emplace_back(MouseEvent { .type = MouseEvent::Type::ScrollUp,
                                                 .x = x,
                                                 .y = y,
                                                 .modifiers = mods,
                                                 .uiHandled = uiHandled });
            }
            else if (buttonBits == 65)
            {
                events.emplace_back(MouseEvent { .type = MouseEvent::Type::ScrollDown,
                                                 .x = x,
                                                 .y = y,
                                                 .modifiers = mods,
                                                 .uiHandled = uiHandled });
            }
            else if (rawButton & 32)
            {
                events.emplace_back(MouseEvent { .type = MouseEvent::Type::Move,
                                                 .button = buttonBits & 3,
                                                 .x = x,
                                                 .y = y,
                                                 .modifiers = mods,
                                                 .uiHandled = uiHandled });
            }
            else if (isRelease)
            {
                events.emplace_back(MouseEvent { .type = MouseEvent::Type::Release,
                                                 .button = buttonBits & 3,
                                                 .x = x,
                                                 .y = y,
                                                 .modifiers = mods,
                                                 .uiHandled = uiHandled });
            }
            else
            {
                events.emplace_back(MouseEvent { .type = MouseEvent::Type::Press,
                                                 .button = buttonBits & 3,
                                                 .x = x,
                                                 .y = y,
                                                 .modifiers = mods,
                                                 .uiHandled = uiHandled });
            }
        }
        return;
    }

    // Check for cursor position report: ESC[row;colR
    if (finalByte == 'R')
    {
        auto const params = parseCsiParams(_paramBuf);
        if (params.size() >= 2)
        {
            events.emplace_back(CursorPositionReport { .row = params[0], .column = params[1] });
        }
        return;
    }

    // Check for bracketed paste start: ESC[200~
    if (finalByte == '~' && _paramBuf == "200")
    {
        _pasteBuf.clear();
        _state = State::PasteBody;
        return;
    }

    // Check for CSIu (Kitty keyboard protocol): ESC[key[:shifted[:base]];modifiers u
    if (finalByte == 'u')
    {
        auto const params = parseCsiParams(_paramBuf);
        auto modifier = (params.size() >= 2) ? decodeModifiers(params[1]) : Modifier::None;

        // Parse the key field's colon-separated subparameters (key:shifted_key:base_layout_key)
        auto const firstSemicolon = _paramBuf.find(';');
        auto const keyFieldStr = (firstSemicolon != std::string_view::npos)
                                     ? std::string_view(_paramBuf).substr(0, firstSemicolon)
                                     : std::string_view(_paramBuf);
        auto const keyInfo = parseKeyField(keyFieldStr);
        auto const keycode = keyInfo.key;

        // When a shifted_key is present and Shift is active, the Shift modifier was
        // "consumed" to produce the shifted character. Use shifted_key as the effective
        // codepoint and strip Shift from modifiers — matching legacy terminal behavior
        // where Shift+3 just sends '#' with no modifier info.
        auto effectiveCodepoint = keycode;
        if (keyInfo.shiftedKey > 0 && hasModifier(modifier, Modifier::Shift))
        {
            effectiveCodepoint = keyInfo.shiftedKey;
            modifier = static_cast<Modifier>(static_cast<std::uint8_t>(modifier)
                                             & ~static_cast<std::uint8_t>(Modifier::Shift));
        }

        switch (keycode)
        {
            case 13: events.emplace_back(KeyEvent { .key = KeyCode::Enter, .modifiers = modifier }); return;
            case 9: events.emplace_back(KeyEvent { .key = KeyCode::Tab, .modifiers = modifier }); return;
            case 127:
                events.emplace_back(KeyEvent { .key = KeyCode::Backspace, .modifiers = modifier });
                return;
            case 27: events.emplace_back(KeyEvent { .key = KeyCode::Escape, .modifiers = modifier }); return;
            default: break;
        }

        // Handle Kitty's special key codes in Private Use Area (57358-57454)
        if (keycode >= 57358 && keycode <= 57454)
        {
            auto mappedKey = std::optional<KeyCode> {};
            switch (keycode)
            {
                    // clang-format off
                // Lock/System keys (57358-57363)
                case 57358: mappedKey = KeyCode::CapsLock; break;
                case 57359: mappedKey = KeyCode::ScrollLock; break;
                case 57360: mappedKey = KeyCode::NumLock; break;
                case 57361: mappedKey = KeyCode::PrintScreen; break;
                case 57362: mappedKey = KeyCode::Pause; break;
                case 57363: mappedKey = KeyCode::Menu; break;

                // Extended function keys F13-F35 (57376-57398)
                case 57376: mappedKey = KeyCode::F13; break;
                case 57377: mappedKey = KeyCode::F14; break;
                case 57378: mappedKey = KeyCode::F15; break;
                case 57379: mappedKey = KeyCode::F16; break;
                case 57380: mappedKey = KeyCode::F17; break;
                case 57381: mappedKey = KeyCode::F18; break;
                case 57382: mappedKey = KeyCode::F19; break;
                case 57383: mappedKey = KeyCode::F20; break;
                case 57384: mappedKey = KeyCode::F21; break;
                case 57385: mappedKey = KeyCode::F22; break;
                case 57386: mappedKey = KeyCode::F23; break;
                case 57387: mappedKey = KeyCode::F24; break;
                case 57388: mappedKey = KeyCode::F25; break;
                case 57389: mappedKey = KeyCode::F26; break;
                case 57390: mappedKey = KeyCode::F27; break;
                case 57391: mappedKey = KeyCode::F28; break;
                case 57392: mappedKey = KeyCode::F29; break;
                case 57393: mappedKey = KeyCode::F30; break;
                case 57394: mappedKey = KeyCode::F31; break;
                case 57395: mappedKey = KeyCode::F32; break;
                case 57396: mappedKey = KeyCode::F33; break;
                case 57397: mappedKey = KeyCode::F34; break;
                case 57398: mappedKey = KeyCode::F35; break;

                // Keypad keys (57399-57427)
                case 57399: mappedKey = KeyCode::Kp0; break;
                case 57400: mappedKey = KeyCode::Kp1; break;
                case 57401: mappedKey = KeyCode::Kp2; break;
                case 57402: mappedKey = KeyCode::Kp3; break;
                case 57403: mappedKey = KeyCode::Kp4; break;
                case 57404: mappedKey = KeyCode::Kp5; break;
                case 57405: mappedKey = KeyCode::Kp6; break;
                case 57406: mappedKey = KeyCode::Kp7; break;
                case 57407: mappedKey = KeyCode::Kp8; break;
                case 57408: mappedKey = KeyCode::Kp9; break;
                case 57409: mappedKey = KeyCode::KpDecimal; break;
                case 57410: mappedKey = KeyCode::KpDivide; break;
                case 57411: mappedKey = KeyCode::KpMultiply; break;
                case 57412: mappedKey = KeyCode::KpSubtract; break;
                case 57413: mappedKey = KeyCode::KpAdd; break;
                case 57414: mappedKey = KeyCode::KpEnter; break;
                case 57415: mappedKey = KeyCode::KpEqual; break;
                case 57416: mappedKey = KeyCode::KpSeparator; break;
                case 57417: mappedKey = KeyCode::KpLeft; break;
                case 57418: mappedKey = KeyCode::KpRight; break;
                case 57419: mappedKey = KeyCode::KpUp; break;
                case 57420: mappedKey = KeyCode::KpDown; break;
                case 57421: mappedKey = KeyCode::KpPageUp; break;
                case 57422: mappedKey = KeyCode::KpPageDown; break;
                case 57423: mappedKey = KeyCode::KpHome; break;
                case 57424: mappedKey = KeyCode::KpEnd; break;
                case 57425: mappedKey = KeyCode::KpInsert; break;
                case 57426: mappedKey = KeyCode::KpDelete; break;
                case 57427: mappedKey = KeyCode::KpBegin; break;

                // Media keys (57428-57440)
                case 57428: mappedKey = KeyCode::MediaPlay; break;
                case 57429: mappedKey = KeyCode::MediaPause; break;
                case 57430: mappedKey = KeyCode::MediaPlayPause; break;
                case 57431: mappedKey = KeyCode::MediaReverse; break;
                case 57432: mappedKey = KeyCode::MediaStop; break;
                case 57433: mappedKey = KeyCode::MediaFastForward; break;
                case 57434: mappedKey = KeyCode::MediaRewind; break;
                case 57435: mappedKey = KeyCode::MediaTrackNext; break;
                case 57436: mappedKey = KeyCode::MediaTrackPrevious; break;
                case 57437: mappedKey = KeyCode::MediaRecord; break;
                case 57438: mappedKey = KeyCode::LowerVolume; break;
                case 57439: mappedKey = KeyCode::RaiseVolume; break;
                case 57440: mappedKey = KeyCode::MuteVolume; break;

                // Modifier keys as key events (57441-57454)
                case 57441: mappedKey = KeyCode::LeftShift; break;
                case 57442: mappedKey = KeyCode::LeftControl; break;
                case 57443: mappedKey = KeyCode::LeftAlt; break;
                case 57444: mappedKey = KeyCode::LeftSuper; break;
                case 57445: mappedKey = KeyCode::LeftHyper; break;
                case 57446: mappedKey = KeyCode::LeftMeta; break;
                case 57447: mappedKey = KeyCode::RightShift; break;
                case 57448: mappedKey = KeyCode::RightControl; break;
                case 57449: mappedKey = KeyCode::RightAlt; break;
                case 57450: mappedKey = KeyCode::RightSuper; break;
                case 57451: mappedKey = KeyCode::RightHyper; break;
                case 57452: mappedKey = KeyCode::RightMeta; break;
                case 57453: mappedKey = KeyCode::IsoLevel3Shift; break;
                case 57454: mappedKey = KeyCode::IsoLevel5Shift; break;
                    // clang-format on

                default: break; // Unknown PUA key in this range
            }

            if (mappedKey)
            {
                auto const cp = resolveNumpadCodepoint(*mappedKey, modifier);
                events.emplace_back(KeyEvent { .key = *mappedKey, .modifiers = modifier, .codepoint = cp });
            }
            // Silently ignore unmapped keys in PUA range
            return;
        }

        // Printable codepoint (but exclude remaining PUA range)
        if (keycode >= 32 && keycode < 0x10000 && (keycode < 57344 || keycode > 63743))
        {
            auto const cp = static_cast<char32_t>(effectiveCodepoint);
            events.emplace_back(
                KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = modifier, .codepoint = cp });
        }
        return;
    }

    // Handle cell size report: CSI 6 ; height ; width t (response to CSI 16 t)
    if (finalByte == 't')
    {
        auto const params = parseCsiParams(_paramBuf);
        if (params.size() >= 3 && params[0] == 6)
        {
            events.emplace_back(CellSizeReport { .height = params[1], .width = params[2] });
            return;
        }
    }

    // Handle color scheme report: CSI ? 997 ; N n
    // N: 1 = dark, 2 = light
    if (finalByte == 'n' && _paramBuf.starts_with("?"))
    {
        auto const privateParams = parseCsiParams(_paramBuf.substr(1));
        if (privateParams.size() >= 2 && privateParams[0] == 997)
        {
            events.emplace_back(ColorSchemeReport { .mode = privateParams[1] });
            return;
        }
    }

    // DECRQM response: CSI ? mode ; status $ y
    // _paramBuf contains "?mode;status$" and finalByte is 'y'.
    if (finalByte == 'y' && _paramBuf.starts_with("?") && _paramBuf.ends_with("$"))
    {
        // Strip the leading '?' and trailing '$' to get "mode;status".
        auto const inner = std::string_view(_paramBuf).substr(1, _paramBuf.size() - 2);
        auto const params = parseCsiParams(inner);
        if (params.size() >= 2)
        {
            events.emplace_back(DecModeReport { .mode = params[0], .status = params[1] });
            return;
        }
    }

    // Win32 input mode: CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
    // All parameters are optional with defaults (Vk=0, Sc=0, Uc=0, Kd=0, Cs=0, Rc=1),
    // matching the Windows Terminal reference implementation.
    if (finalByte == '_')
    {
        auto const params = parseCsiParams(_paramBuf);
        if (params.empty())
            return; // Completely empty — nothing to process

        auto const vkCode = paramAt(params, 0);
        // auto const scanCode = paramAt(params, 1); // unused
        auto const unicodeChar = paramAt(params, 2);
        auto const keyDown = paramAt(params, 3);
        auto const controlKeyState = paramAt(params, 4);
        // Repeat count defaults to 1. Windows Terminal sends each auto-repeat
        // as a separate sequence with repeatCount=1, so this field is always 1 in practice.
        // auto const repeatCount = paramAt(params, 5, 1);

        // Skip key-up events
        if (keyDown == 0)
            return;

        // Skip modifier-only key presses
        if (isWin32ModifierOnlyVk(vkCode))
            return;

        auto const mods = decodeWin32Modifiers(controlKeyState);
        auto const isAltGr = isAltGrKeyState(controlKeyState);

        // A character outside the BMP is two keys, one per UTF-16 surrogate (core-cpp#20): the high
        // half is held until the key carrying the low half, which may come in a later read, and
        // only the pair is a character. A half that cannot be paired decodes to nothing, as an
        // encoded surrogate does in the UTF-8 path.
        if (unicodeChar >= 0xD800 && unicodeChar <= 0xDBFF)
        {
            _pendingHighSurrogate = static_cast<char16_t>(unicodeChar);
            return;
        }
        if (unicodeChar >= 0xDC00 && unicodeChar <= 0xDFFF)
        {
            auto const high = std::exchange(_pendingHighSurrogate, char16_t { 0 });
            if (high == 0)
                return;
            auto const cp =
                static_cast<char32_t>(0x10000 + (((high - 0xD800) << 10) | (unicodeChar - 0xDC00)));
            events.emplace_back(
                KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = mods, .codepoint = cp });
            return;
        }
        _pendingHighSurrogate = 0;

        // Try special key mapping (Backspace, Enter, arrows, F-keys, etc.)
        if (auto const mapped = mapWin32VkToKeyCode(vkCode))
        {
            auto const cp = resolveNumpadCodepoint(*mapped, mods, unicodeChar);
            events.emplace_back(KeyEvent { .key = *mapped, .modifiers = mods, .codepoint = cp });
            return;
        }

        // VK_SPACE: always emit U+0020 (UC may be 0 with Ctrl held)
        if (vkCode == vk::Space)
        {
            constexpr auto Cp = U' ';
            events.emplace_back(
                KeyEvent { .key = keyCodeFromCodepoint(Cp), .modifiers = mods, .codepoint = Cp });
            return;
        }

        // VK A-Z (0x41-0x5A): emit lowercase letter, Shift reflected in modifiers.
        // When AltGr is active, prefer the unicodeChar field which contains the composed
        // character (e.g., AltGr+E → U+20AC '€' on German keyboards).
        if (vkCode >= 0x41 && vkCode <= 0x5A)
        {
            if (isAltGr && unicodeChar >= 32)
            {
                auto const cp = static_cast<char32_t>(unicodeChar);
                events.emplace_back(
                    KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = mods, .codepoint = cp });
                return;
            }
            auto const cp = static_cast<char32_t>(static_cast<unsigned>(vkCode) - 0x41u + U'a');
            events.emplace_back(
                KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = mods, .codepoint = cp });
            return;
        }

        // VK 0-9 (0x30-0x39): use unicodeChar when printable — it already reflects the active
        // keyboard layout and Shift state (e.g. Shift+9 → '(' on US, AltGr+2 → '²' on German).
        // Unlike letters (where Shift+a universally means 'A'), Shift+digit mappings are
        // layout-dependent, so the consumer cannot derive the shifted character from the VK code.
        if (vkCode >= 0x30 && vkCode <= 0x39)
        {
            auto const cp =
                (unicodeChar >= 32) ? static_cast<char32_t>(unicodeChar) : static_cast<char32_t>(vkCode);
            events.emplace_back(
                KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = mods, .codepoint = cp });
            return;
        }

        // Fall back to Unicode character for remaining printable keys
        if (unicodeChar >= 32)
        {
            auto const cp = static_cast<char32_t>(unicodeChar);
            events.emplace_back(
                KeyEvent { .key = keyCodeFromCodepoint(cp), .modifiers = mods, .codepoint = cp });
            return;
        }

        // Silently ignore anything else
        return;
    }

    // Primary Device Attributes (DA1) response: CSI ? p1 ; p2 ; ... c
    if (finalByte == 'c' && _paramBuf.starts_with("?"))
    {
        events.emplace_back(DeviceAttributesReport { .attributes = parseCsiParams(_paramBuf.substr(1)) });
        return;
    }

    // Standard CSI sequences (cursor keys, function keys, etc.)
    // Skip sequences with private markers (>, ?, but not <)
    if (!_paramBuf.empty() && (_paramBuf[0] == '>' || _paramBuf[0] == '?'))
        return;

    auto const params = parseCsiParams(_paramBuf);
    if (auto key = mapCsiKey(finalByte, params))
    {
        events.emplace_back(*key);
    }
}

void VtParser::processDcsEntry(std::uint8_t byte, std::vector<InputEvent>& events)
{
    // DCS parameter/intermediate bytes: 0x20-0x7E range before the body starts.
    // The "final byte" of the DCS header is the first byte >= 0x40 in the range [0x40,0x7E].
    // Parameter bytes: 0x30-0x3F (digits, semicolons, etc.)
    // Intermediate bytes: 0x20-0x2F
    if (byte >= 0x20 && byte <= 0x3F)
    {
        _dcsBuf += static_cast<char>(byte);
        std::ignore = abandonIfOverlong(_dcsBuf, MaxDcsBuffer);
        return;
    }

    // Final byte of DCS header — transition to body collection.
    if (byte >= 0x40 && byte <= 0x7E)
    {
        _dcsBuf += static_cast<char>(byte);
        _state = State::DcsBody;
        std::ignore = abandonIfOverlong(_dcsBuf, MaxDcsBuffer);
        return;
    }

    // ESC in DcsEntry starts potential ST (ESC \) — but could also be malformed.
    // For robustness, if we see ESC here, check next byte in DcsBody.
    if (byte == 0x1B)
    {
        _state = State::DcsBody;
        // Re-feed this byte in DcsBody to check for ST.
        processDcsBody(byte, events);
        return;
    }

    // Unexpected byte — abort DCS, return to ground.
    _dcsBuf.clear();
    _state = State::Ground;
}

void VtParser::processDcsBody(std::uint8_t byte, std::vector<InputEvent>& events)
{
    _dcsBuf += static_cast<char>(byte);

    // Check for ST (String Terminator): ESC \ (0x1B 0x5C)
    if (_dcsBuf.ends_with(StringTerminator))
    {
        // Remove the trailing ESC \ from the payload.
        _dcsBuf.resize(_dcsBuf.size() - StringTerminator.size());
        events.emplace_back(DcsResponse { .payload = std::move(_dcsBuf) });
        _dcsBuf.clear();
        _state = State::Ground;
        return;
    }

    std::ignore = abandonIfOverlong(_dcsBuf, MaxDcsBuffer);
}

void VtParser::processOscBody(std::uint8_t byte, std::vector<InputEvent>& events)
{
    // BEL ends an OSC as ST does; xterm accepts both, and terminals answer with either.
    if (byte != Bell)
    {
        _oscBuf += static_cast<char>(byte);
        if (!_oscBuf.ends_with(StringTerminator))
        {
            std::ignore = abandonIfOverlong(_oscBuf, MaxOscBuffer);
            return;
        }
        _oscBuf.resize(_oscBuf.size() - StringTerminator.size());
    }
    events.emplace_back(OscResponse { .payload = std::exchange(_oscBuf, {}) });
    _state = State::Ground;
}

void VtParser::emitCodepoint(char32_t cp, std::vector<InputEvent>& events)
{
    events.emplace_back(KeyEvent { .key = keyCodeFromCodepoint(cp), .codepoint = cp });
}

void VtParser::emitUtf8(std::vector<InputEvent>& events)
{
    // Decode UTF-8 sequence to a single codepoint
    auto const* const bytes = reinterpret_cast<std::uint8_t const*>(_utf8Buf.data());
    auto const len = _utf8Buf.size();
    auto cp = char32_t { 0 };

    if (len == 2)
        cp = static_cast<char32_t>(((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F));
    else if (len == 3)
        cp = static_cast<char32_t>(((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F));
    else if (len == 4)
        cp = static_cast<char32_t>(((bytes[0] & 0x07) << 18) | ((bytes[1] & 0x3F) << 12)
                                   | ((bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F));

    // Arithmetic alone decodes things no UTF-8 decoder may produce, and each is dropped here as an
    // invalid lead byte is dropped above: an overlong encoding (the shortest form is the only
    // valid one), an encoded surrogate -- which is what CESU-8 input decodes to, and what the
    // Windows console arm used to deliver for every character outside the BMP (core-cpp#20) -- and
    // a value above U+10FFFF.
    auto const overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
    auto const surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (cp == 0 || overlong || surrogate || cp > 0x10FFFF)
        return;
    events.emplace_back(KeyEvent { .key = keyCodeFromCodepoint(cp), .codepoint = cp });
}

} // namespace core::tui
