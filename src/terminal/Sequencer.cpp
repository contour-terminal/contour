/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Sequencer.h>
#include <terminal/primitives.h>
#include <terminal/logging.h>

#include <terminal/Functions.h>
#include <terminal/SixelParser.h>
#include <terminal/Screen.h>
#include <terminal/logging.h>

#include <crispy/algorithm.h>
#include <crispy/base64.h>
#include <crispy/escape.h>
#include <crispy/utils.h>

#include <unicode/utf8.h>
#include <unicode/convert.h>

#include <fmt/format.h>

#include <array>
#include <iostream>             // error logging
#include <cstdlib>
#include <iterator>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using crispy::Size;
using std::array;
using std::clamp;
using std::distance;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::make_unique;
using std::max;
using std::min;
using std::nullopt;
using std::optional;
using std::pair;
using std::runtime_error;
using std::shared_ptr;
using std::string_view;
using std::stoi;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::u32string_view;
using std::vector;

using namespace std::string_view_literals;

namespace terminal {

namespace // {{{ helpers
{
    /// @returns parsed tuple with OSC code and offset to first data parameter byte.
    pair<int, int> parseOSC(string const& _data)
    {
        int code = 0;
        size_t i = 0;

        while (i < _data.size() && isdigit(_data[i]))
            code = code * 10 + _data[i++] - '0';

        if (i == 0 && !_data.empty() && _data[0] != ';')
        {
            // such as 'L' is encoded as -'L'
            code = -_data[0];
            ++i;
        }

        if (i < _data.size() && _data[i] == ';')
            ++i;

        return pair{code, i};
    }

    // optional<CharsetTable> getCharsetTableForCode(std::string const& _intermediate)
    // {
    //     if (_intermediate.size() != 1)
    //         return nullopt;
    //
    //     char32_t const code = _intermediate[0];
    //     switch (code)
    //     {
    //         case '(':
    //             return {CharsetTable::G0};
    //         case ')':
    //         case '-':
    //             return {CharsetTable::G1};
    //         case '*':
    //         case '.':
    //             return {CharsetTable::G2};
    //         case '+':
    //         case '/':
    //             return {CharsetTable::G3};
    //         default:
    //             return nullopt;
    //     }
    // }
} // }}}

namespace impl // {{{ some command generator helpers
{
    ApplyResult setAnsiMode(Sequence const& _seq, size_t _modeIndex, bool _enable, Screen& _screen)
	{
		switch (_seq.param(_modeIndex))
		{
			case 2:  // (AM) Keyboard Action Mode
				return ApplyResult::Unsupported;
			case 4:  // (IRM) Insert Mode
                _screen.setMode(AnsiMode::Insert, _enable);
                return ApplyResult::Ok;
			case 12:  // (SRM) Send/Receive Mode
			case 20:  // (LNM) Automatic Newline
			default:
				return ApplyResult::Unsupported;
		}
	}

    optional<DECMode> toDECMode(int _value)
    {
        switch (_value)
        {
            case 1: return DECMode::UseApplicationCursorKeys;
            case 2: return DECMode::DesignateCharsetUSASCII;
            case 3: return DECMode::Columns132;
            case 4: return DECMode::SmoothScroll;
            case 5: return DECMode::ReverseVideo;
            case 6: return DECMode::Origin;
            case 7: return DECMode::AutoWrap;
            // TODO: Ps = 8  -> Auto-repeat Keys (DECARM), VT100.
            case 9: return DECMode::MouseProtocolX10;
            case 10: return DECMode::ShowToolbar;
            case 12: return DECMode::BlinkingCursor;
            case 19: return DECMode::PrinterExtend;
            case 25: return DECMode::VisibleCursor;
            case 30: return DECMode::ShowScrollbar;
            // TODO: Ps = 3 5  -> Enable font-shifting functions (rxvt).
            // IGNORE? Ps = 3 8  -> Enter Tektronix Mode (DECTEK), VT240, xterm.
            // TODO: Ps = 4 0  -> Allow 80 -> 132 Mode, xterm.
            case 40: return DECMode::AllowColumns80to132;
            // IGNORE: Ps = 4 1  -> more(1) fix (see curses resource).
            // TODO: Ps = 4 2  -> Enable National Replacement Character sets (DECNRCM), VT220.
            // TODO: Ps = 4 4  -> Turn On Margin Bell, xterm.
            // TODO: Ps = 4 5  -> Reverse-wraparound Mode, xterm.
            case 46: return DECMode::DebugLogging;
            case 47: return DECMode::UseAlternateScreen;
            // TODO: Ps = 6 6  -> Application keypad (DECNKM), VT320.
            // TODO: Ps = 6 7  -> Backarrow key sends backspace (DECBKM), VT340, VT420.  This sets the backarrowKey resource to "true".
            case 69: return DECMode::LeftRightMargin;
            case 80: return DECMode::SixelScrolling;
            case 1000: return DECMode::MouseProtocolNormalTracking;
            case 1001: return DECMode::MouseProtocolHighlightTracking;
            case 1002: return DECMode::MouseProtocolButtonTracking;
            case 1003: return DECMode::MouseProtocolAnyEventTracking;
            case 1004: return DECMode::FocusTracking;
            case 1005: return DECMode::MouseExtended;
            case 1006: return DECMode::MouseSGR;
            case 1007: return DECMode::MouseAlternateScroll;
            case 1015: return DECMode::MouseURXVT;
            case 1047: return DECMode::UseAlternateScreen;
            case 1048: return DECMode::SaveCursor;
            case 1049: return DECMode::ExtendedAltScreen;
            case 2004: return DECMode::BracketedPaste;
            case 2026: return DECMode::BatchedRendering;
            case 2027: return DECMode::TextReflow;
        }
        return nullopt;
    }

	ApplyResult setModeDEC(Sequence const& _seq, size_t _modeIndex, bool _enable, Screen& _screen)
	{
        if (auto const modeOpt = toDECMode(_seq.param(_modeIndex)); modeOpt.has_value())
        {
            _screen.setMode(modeOpt.value(), _enable);
            return ApplyResult::Ok;
        }
        return ApplyResult::Invalid;
	}

    optional<RGBColor> parseColor(string_view const& _value)
    {
        try
        {
            // "rgb:RR/GG/BB"
            //  0123456789a
            if (_value.size() == 12 && _value.substr(0, 4) == "rgb:" && _value[6] == '/' && _value[9] == '/')
            {
                auto const r = crispy::to_integer<16, uint8_t>(_value.substr(4, 2));
                auto const g = crispy::to_integer<16, uint8_t>(_value.substr(7, 2));
                auto const b = crispy::to_integer<16, uint8_t>(_value.substr(10, 2));
                return RGBColor{*r, *g, *b};
            }
            return std::nullopt;
        }
        catch (...)
        {
            // that will be a formatting error in stoul() then.
            return std::nullopt;
        }
    }

    Color parseColor(Sequence const& _seq, size_t* pi)
	{
        // We are at parameter index `i`.
        //
        // It may now follow:
        // - ":2::r:g:b"        RGB color
        // - ":3:F:C:M:Y"       CMY color  (F is scaling factor, what is max? 100 or 255?)
        // - ":4:F:C:M:Y:K"     CMYK color (F is scaling factor, what is max? 100 or 255?)
        // - ":5:P"
        // Sub-parameters can also be delimited with ';' and thus are no sub-parameters per-se.
        size_t i = *pi;
        if (_seq.subParameterCount(i) >= 1)
        {
            switch (_seq.subparam(i, 0))
            {
                case 2: // ":2::R:G:B" and ":2:R:G:B"
                {
                    auto const len = _seq.subParameterCount(i);
                    if (len == 4 || len == 5)
                    {
                        // NB: subparam(i, 1) may be ignored
                        auto const r = _seq.subparam(i, len - 3);
                        auto const g = _seq.subparam(i, len - 2);
                        auto const b = _seq.subparam(i, len - 1);
                        if (r <= 255 && g <= 255 && b <= 255)
                        {
                            *pi = i + 1;
                            return Color{RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)} };
                        }
                    }
                    break;
                }
                case 3: // ":3:F:C:M:Y" (TODO)
                case 4: // ":4:F:C:M:Y:K" (TODO)
                    break;
                case 5: // ":5:P"
                    if (auto const P = _seq.subparam(i, 1); P <= 255)
                    {
                        *pi = i + 1;
                        return static_cast<IndexedColor>(P);
                    }
                    break;
                default:
                    break; // XXX invalid sub parameter
            }
        }

        // Compatibility mode, colors using ';' instead of ':'.
		if (i + 1 < _seq.parameterCount())
		{
			++i;
			auto const mode = _seq.param(i);
			if (mode == 5)
			{
				if (i + 1 < _seq.parameterCount())
				{
					++i;
					auto const value = _seq.param(i);
					if (i <= 255)
                    {
						*pi = i;
                        return static_cast<IndexedColor>(value);
                    }
					else
                        {} // TODO: _seq.logInvalidCSI("Invalid color indexing.");
				}
				else
                    {} // TODO: _seq.logInvalidCSI("Missing color index.");
			}
			else if (mode == 2)
			{
				if (i + 3 < _seq.parameterCount())
				{
					auto const r = _seq.param(i + 1);
					auto const g = _seq.param(i + 2);
					auto const b = _seq.param(i + 3);
					i += 3;
					if (r <= 255 && g <= 255 && b <= 255)
                    {
						*pi = i;
                        return RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                    }
					else
                        {} // TODO: _seq.logInvalidCSI("RGB color out of range.");
				}
				else
                    {} // TODO: _seq.logInvalidCSI("Invalid color mode.");
			}
			else
                {} // TODO: _seq.logInvalidCSI("Invalid color mode.");
		}
		else
            {} // TODO: _seq.logInvalidCSI("Invalid color indexing.");

        // failure case, skip this argument
        *pi = i + 1;
        return Color{};
	}

	ApplyResult dispatchSGR(Sequence const& _seq, Screen& _screen)
	{
        if (_seq.parameterCount() == 0)
        {
           _screen.setGraphicsRendition(GraphicsRendition::Reset);
            return ApplyResult::Ok;
        }

		for (size_t i = 0; i < _seq.parameterCount(); ++i)
		{
			switch (_seq.param(i))
			{
				case 0: _screen.setGraphicsRendition(GraphicsRendition::Reset); break;
				case 1: _screen.setGraphicsRendition(GraphicsRendition::Bold); break;
				case 2: _screen.setGraphicsRendition(GraphicsRendition::Faint); break;
				case 3: _screen.setGraphicsRendition(GraphicsRendition::Italic); break;
				case 4:
                    if (_seq.subParameterCount(i) == 1)
                    {
                        switch (_seq.subparam(i, 0))
                        {
                            case 0: _screen.setGraphicsRendition(GraphicsRendition::NoUnderline); break; // 4:0
                            case 1: _screen.setGraphicsRendition(GraphicsRendition::Underline); break; // 4:1
                            case 2: _screen.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break; // 4:2
                            case 3: _screen.setGraphicsRendition(GraphicsRendition::CurlyUnderlined); break; // 4:3
                            case 4: _screen.setGraphicsRendition(GraphicsRendition::DottedUnderline); break; // 4:4
                            case 5: _screen.setGraphicsRendition(GraphicsRendition::DashedUnderline); break; // 4:5
                            default: _screen.setGraphicsRendition(GraphicsRendition::Underline); break;
                        }
                    }
                    else
                        _screen.setGraphicsRendition(GraphicsRendition::Underline);
					break;
				case 5: _screen.setGraphicsRendition(GraphicsRendition::Blinking); break;
				case 7: _screen.setGraphicsRendition(GraphicsRendition::Inverse); break;
				case 8: _screen.setGraphicsRendition(GraphicsRendition::Hidden); break;
				case 9: _screen.setGraphicsRendition(GraphicsRendition::CrossedOut); break;
				case 21: _screen.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break;
				case 22: _screen.setGraphicsRendition(GraphicsRendition::Normal); break;
				case 23: _screen.setGraphicsRendition(GraphicsRendition::NoItalic); break;
				case 24: _screen.setGraphicsRendition(GraphicsRendition::NoUnderline); break;
				case 25: _screen.setGraphicsRendition(GraphicsRendition::NoBlinking); break;
				case 27: _screen.setGraphicsRendition(GraphicsRendition::NoInverse); break;
				case 28: _screen.setGraphicsRendition(GraphicsRendition::NoHidden); break;
				case 29: _screen.setGraphicsRendition(GraphicsRendition::NoCrossedOut); break;
				case 30: _screen.setForegroundColor(IndexedColor::Black); break;
				case 31: _screen.setForegroundColor(IndexedColor::Red); break;
				case 32: _screen.setForegroundColor(IndexedColor::Green); break;
				case 33: _screen.setForegroundColor(IndexedColor::Yellow); break;
				case 34: _screen.setForegroundColor(IndexedColor::Blue); break;
				case 35: _screen.setForegroundColor(IndexedColor::Magenta); break;
				case 36: _screen.setForegroundColor(IndexedColor::Cyan); break;
				case 37: _screen.setForegroundColor(IndexedColor::White); break;
				case 38: _screen.setForegroundColor(parseColor(_seq, &i)); break;
				case 39: _screen.setForegroundColor(DefaultColor()); break;
				case 40: _screen.setBackgroundColor(IndexedColor::Black); break;
				case 41: _screen.setBackgroundColor(IndexedColor::Red); break;
				case 42: _screen.setBackgroundColor(IndexedColor::Green); break;
				case 43: _screen.setBackgroundColor(IndexedColor::Yellow); break;
				case 44: _screen.setBackgroundColor(IndexedColor::Blue); break;
				case 45: _screen.setBackgroundColor(IndexedColor::Magenta); break;
				case 46: _screen.setBackgroundColor(IndexedColor::Cyan); break;
				case 47: _screen.setBackgroundColor(IndexedColor::White); break;
				case 48: _screen.setBackgroundColor(parseColor(_seq, &i)); break;
				case 49: _screen.setBackgroundColor(DefaultColor()); break;
                case 51: _screen.setGraphicsRendition(GraphicsRendition::Framed); break;
                case 53: _screen.setGraphicsRendition(GraphicsRendition::Overline); break;
                case 54: _screen.setGraphicsRendition(GraphicsRendition::NoFramed); break;
                case 55: _screen.setGraphicsRendition(GraphicsRendition::NoOverline); break;
                // 58 is reserved, but used for setting underline/decoration colors by some other VTEs (such as mintty, kitty, libvte)
                case 58: _screen.setUnderlineColor(parseColor(_seq, &i)); break;
				case 90: _screen.setForegroundColor(BrightColor::Black); break;
				case 91: _screen.setForegroundColor(BrightColor::Red); break;
				case 92: _screen.setForegroundColor(BrightColor::Green); break;
				case 93: _screen.setForegroundColor(BrightColor::Yellow); break;
				case 94: _screen.setForegroundColor(BrightColor::Blue); break;
				case 95: _screen.setForegroundColor(BrightColor::Magenta); break;
				case 96: _screen.setForegroundColor(BrightColor::Cyan); break;
				case 97: _screen.setForegroundColor(BrightColor::White); break;
				case 100: _screen.setBackgroundColor(BrightColor::Black); break;
				case 101: _screen.setBackgroundColor(BrightColor::Red); break;
				case 102: _screen.setBackgroundColor(BrightColor::Green); break;
				case 103: _screen.setBackgroundColor(BrightColor::Yellow); break;
				case 104: _screen.setBackgroundColor(BrightColor::Blue); break;
				case 105: _screen.setBackgroundColor(BrightColor::Magenta); break;
				case 106: _screen.setBackgroundColor(BrightColor::Cyan); break;
				case 107: _screen.setBackgroundColor(BrightColor::White); break;
				default: break; // TODO: logInvalidCSI("Invalid SGR number: {}", _seq.param(i));
			}
		}
		return ApplyResult::Ok;
	}

    ApplyResult CPR(Sequence const& _seq, Screen& _screen)
    {
        switch (_seq.param(0))
        {
            case 5: _screen.deviceStatusReport(); return ApplyResult::Ok;
            case 6: _screen.reportCursorPosition(); return ApplyResult::Ok;
            default: return ApplyResult::Unsupported;
        }
    }

    ApplyResult DECRQPSR(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() != 1)
            return ApplyResult::Invalid; // -> error
        else if (_seq.param(0) == 1)
            // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
            // TODO return emitCommand<RequestCursorState>(); // or call it with ...Detailed?
            return ApplyResult::Invalid;
        else if (_seq.param(0) == 2)
        {
            _screen.requestTabStops();
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult DECSCUSR(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() <= 1)
        {
            switch (_seq.param_or(0, Sequence::Parameter{1}))
            {
                case 0:
                case 1: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Block); break;
                case 2: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Block); break;
                case 3: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Underscore); break;
                case 4: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Underscore); break;
                case 5: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Bar); break;
                case 6: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Bar); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult ED(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() == 0)
            _screen.clearToEndOfScreen();
        else
        {
            for (size_t i = 0; i < _seq.parameterCount(); ++i)
            {
                switch (_seq.param(i))
                {
                    case 0: _screen.clearToEndOfScreen(); break;
                    case 1: _screen.clearToBeginOfScreen(); break;
                    case 2: _screen.clearScreen(); break;
                    case 3: _screen.clearScrollbackBuffer(); break;
                }
            }
        }
        return ApplyResult::Ok;
    }

    ApplyResult EL(Sequence const& _seq, Screen& _screen)
    {
        switch (_seq.param_or(0, Sequence::Parameter{0}))
        {
            case 0: _screen.clearToEndOfLine(); break;
            case 1: _screen.clearToBeginOfLine(); break;
            case 2: _screen.clearLine(); break;
            default: return ApplyResult::Invalid;
        }
        return ApplyResult::Ok;
    }

    ApplyResult TBC(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() != 1)
        {
            _screen.horizontalTabClear(HorizontalTabClear::UnderCursor);
            return ApplyResult::Ok;
        }

        switch (_seq.param(0))
        {
            case 0: _screen.horizontalTabClear(HorizontalTabClear::UnderCursor); break;
            case 3: _screen.horizontalTabClear(HorizontalTabClear::AllTabs); break;
            default: return ApplyResult::Invalid;
        }
        return ApplyResult::Ok;
    }

    inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(std::string_view const& s)
    {
        return crispy::splitKeyValuePairs(s, ':');
    }

    ApplyResult setOrRequestDynamicColor(Sequence const& _seq, Screen& _screen, DynamicColorName _name)
    {
        auto const& value = _seq.intermediateCharacters();
        if (value == "?")
            _screen.requestDynamicColor(_name);
        else if (auto color = parseColor(value); color.has_value())
            _screen.setDynamicColor(_name, color.value());
        else
            return ApplyResult::Invalid;

        return ApplyResult::Ok;
    }

    bool queryOrSetColorPalette(string_view _text,
                                std::function<void(uint8_t)> _queryColor,
                                std::function<void(uint8_t, RGBColor)> _setColor)
    {
        // Sequence := [Param (';' Param)*]
        // Param    := Index ';' Query | Set
        // Index    := DIGIT+
        // Query    := ?'
        // Set      := 'rgb:' Hex8 '/' Hex8 '/' Hex8
        // Hex8     := [0-9A-Za-z] [0-9A-Za-z]
        // DIGIT    := [0-9]
        int index = -1;
        return crispy::split(
            _text,
            ';',
            [&](string_view value) {
                if (index < 0)
                {
                    index = crispy::to_integer<10>(value).value_or(-1);
                    if (!(0 <= index && index < 0xFF))
                        return false;
                }
                else if (value == "?"sv)
                {
                    _queryColor(index);
                    index = -1;
                }
                else if (auto const color = parseColor(value))
                {
                    _setColor(index, color.value());
                    index = -1;
                }
                else
                    return false;

                return true;
            }
        );
    }

    ApplyResult RCOLPAL(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.intermediateCharacters().empty())
        {
            _screen.colorPalette() = _screen.defaultColorPalette();
            return ApplyResult::Ok;
        }

        auto const index = crispy::to_integer<10, uint8_t>(_seq.intermediateCharacters());
        if (!index.has_value())
            return ApplyResult::Invalid;

        _screen.colorPalette().palette[*index] = _screen.defaultColorPalette().palette[*index];

        return ApplyResult::Ok;
    }

    ApplyResult SETCOLPAL(Sequence const& _seq, Screen& _screen)
    {
        bool const ok = queryOrSetColorPalette(
            _seq.intermediateCharacters(),
            [&](uint8_t index) {
                auto const color = _screen.colorPalette().palette.at(index);
                _screen.reply("\e]4;rgb:{:02x}/{:02x}/{:02x}\\", color.red, color.green, color.blue);
            },
            [&](uint8_t index, RGBColor color) {
                _screen.colorPalette().palette.at(index) = color;
            }
        );

        return ok ? ApplyResult::Ok : ApplyResult::Invalid;
    }

    int toInt(string_view _value)
    {
        int out = 0;
        for (auto const ch : _value)
        {
            if (!(ch >= '0' && ch <= '9'))
                return 0;

            out = out * 10 + (ch - '0');
        }
        return out;
    }

    string autoFontFace(string_view _value, string_view _regular, string_view _style)
    {
        (void) _style;
        (void) _regular;
        return string(_value);
        // if (!_value.empty() && _value != "auto")
        //     return string(_value);
        // else
        //     return fmt::format("{}:style={}", _regular, _style);
    }

    ApplyResult setAllFont(Sequence const& _seq, Screen& _screen)
    {
        // [read]  OSC 60 ST
        // [write] OSC 60 ; size ; regular ; bold ; italic ; bold italic ST
        auto const& params = _seq.intermediateCharacters();
        auto const splits = crispy::split(params, ';');
        auto const param = [&](int _index) -> string_view {
            if (_index < int(splits.size()))
                return splits.at(_index);
            else
                return string_view{};
        };
        auto const emptyParams = [&]() -> bool {
            for (auto const& x : splits)
                if (!x.empty())
                    return false;
            return true;
        }();
        if (emptyParams)
        {
            auto const fonts = _screen.eventListener().getFontDef();
            _screen.reply(
                "\033]60;{};{};{};{};{};{}\033\\",
                int(fonts.size * 100), // precission-shift
                fonts.regular,
                fonts.bold,
                fonts.italic,
                fonts.boldItalic,
                fonts.emoji
            );
        }
        else
        {
            auto const size = double(toInt(param(0))) / 100.0;
            auto const regular = string(param(1));
            auto const bold = string(param(2));
            auto const italic = string(param(3));
            auto const boldItalic = string(param(4));
            auto const emoji = string(param(5));
            _screen.eventListener().setFontDef(FontDef{
                size,
                regular,
                bold,
                italic,
                boldItalic,
                emoji
            });
        }
        return ApplyResult::Ok;
    }

    ApplyResult setFont(Sequence const& _seq, Screen& _screen)
    {
        auto const& params = _seq.intermediateCharacters();
        auto const splits = crispy::split(params, ';');

        if (splits.size() != 1)
            return ApplyResult::Invalid;

        if (splits[0] != "?"sv)
        {
            auto fontDef = FontDef{};
            fontDef.regular = splits[0];
            _screen.eventListener().setFontDef(fontDef);
        }
        else
        {
            auto const fonts = _screen.eventListener().getFontDef();
            _screen.reply("\033]50;{}\033\\", fonts.regular);
        }

        return ApplyResult::Ok;
    }

    ApplyResult clipboard(Sequence const& _seq, Screen& _screen)
    {
        // Only setting clipboard contents is supported, not reading.
        auto const& params = _seq.intermediateCharacters();
        if (auto const splits = crispy::split(params, ';'); splits.size() == 2 && splits[0] == "c")
        {
            _screen.eventListener().copyToClipboard(crispy::base64::decode(splits[1]));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult NOTIFY(Sequence const& _seq, Screen& _screen)
    {
        auto const& value = _seq.intermediateCharacters();
        if (auto const splits = crispy::split(value, ';'); splits.size() == 3 && splits[0] == "notify")
        {
            _screen.notify(string(splits[1]), string(splits[2]));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Unsupported;
    }

    ApplyResult SETCWD(Sequence const& _seq, Screen& _screen)
    {
        string const& url = _seq.intermediateCharacters();
        _screen.setCurrentWorkingDirectory(url);
        return ApplyResult::Ok;
    }

    ApplyResult CAPTURE(Sequence const& _seq, Screen& _screen)
    {
        // CSI Mode ; [; Count] t
        //
        // Mode: 0 = physical lines
        //       1 = logical lines (unwrapped)
        //
        // Count: number of lines to capture from main page aera's bottom upwards
        //        If omitted or 0, the main page area's line count will be used.

        auto const logicalLines = _seq.param_or(0, 0);
        if (logicalLines != 0 && logicalLines != 1)
            return ApplyResult::Invalid;

        auto const lineCount = _seq.param_or(1, *_screen.size().lines);

        _screen.eventListener().requestCaptureBuffer(lineCount, logicalLines);

        return ApplyResult::Ok;
    }

    ApplyResult HYPERLINK(Sequence const& _seq, Screen& _screen)
    {
        auto const& value = _seq.intermediateCharacters();
        // hyperlink_OSC ::= OSC '8' ';' params ';' URI
        // params := pair (':' pair)*
        // pair := TEXT '=' TEXT
        if (auto const pos = value.find(';'); pos != value.npos)
        {
            auto const paramsStr = value.substr(0, pos);
            auto const params = parseSubParamKeyValuePairs(paramsStr);

            auto id = string{};
            if (auto const p = params.find("id"); p != params.end())
                id = p->second;

            if (pos + 1 != value.size())
                _screen.hyperlink(id, value.substr(pos + 1));
            else
                _screen.hyperlink(string{id}, string{});

            return ApplyResult::Ok;
        }
        else
            _screen.hyperlink(string{}, string{});

        return ApplyResult::Ok;
    }

    ApplyResult saveDECModes(Sequence const& _seq, Screen& _screen)
    {
        vector<DECMode> modes;
        for (size_t i = 0; i < _seq.parameterCount(); ++i)
            if (optional<DECMode> mode = toDECMode(_seq.param(i)); mode.has_value())
                modes.push_back(mode.value());
        _screen.saveModes(modes);
        return ApplyResult::Ok;
    }

    ApplyResult restoreDECModes(Sequence const& _seq, Screen& _screen)
    {
        vector<DECMode> modes;
        for (size_t i = 0; i < _seq.parameterCount(); ++i)
            if (optional<DECMode> mode = toDECMode(_seq.param(i)); mode.has_value())
                modes.push_back(mode.value());
        _screen.restoreModes(modes);
        return ApplyResult::Ok;
    }

    ApplyResult WINDOWMANIP(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() == 3)
        {
            switch (_seq.param(0))
            {
                case 4: // resize in pixel units
                    _screen.eventListener().resizeWindow(ImageSize{
                        Width(_seq.param(2)),
                        Height(_seq.param(1))
                    });
                    break;
                case 8: // resize in cell units
                    _screen.eventListener().resizeWindow(PageSize{
                        LineCount(_seq.param(1)),
                        ColumnCount(_seq.param(2))
                    });
                    break;
                case 22: _screen.saveWindowTitle(); break;
                case 23: _screen.restoreWindowTitle(); break;
                default: return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        else if (_seq.parameterCount() == 1)
        {
            switch (_seq.param(0))
            {
                case 4:
                case 8:
                    // this means, resize to full display size
                    // TODO: just create a dedicated callback for fulscreen resize!
                    _screen.eventListener().resizeWindow(ImageSize{});
                    break;
                case 14:
                    if (_seq.parameterCount() == 2 && _seq.param(1) == 2)
                        _screen.requestPixelSize(RequestPixelSize::WindowArea);   // CSI 14 ; 2 t
                    else
                        _screen.requestPixelSize(RequestPixelSize::TextArea);     // CSI 14 t
                    break;
                case 16:
                    _screen.requestPixelSize(RequestPixelSize::CellArea);
                    break;
                case 18:
                    _screen.requestCharacterSize(RequestPixelSize::TextArea);
                    break;
                case 19:
                    _screen.requestCharacterSize(RequestPixelSize::WindowArea);
                    break;
                default:
                    return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Unsupported;
    }

    ApplyResult XTSMGRAPHICS(Sequence const& _seq, Screen& _screen)
    {
        auto const Pi = _seq.param<unsigned>(0);
        auto const Pa = _seq.param<unsigned>(1);
        auto const Pv = _seq.param_or<unsigned>(2, 0);
        auto const Pu = _seq.param_or<unsigned>(3, 0);

        auto const item = [&]() -> optional<XtSmGraphics::Item> {
            switch (Pi) {
                case 1: return XtSmGraphics::Item::NumberOfColorRegisters;
                case 2: return XtSmGraphics::Item::SixelGraphicsGeometry;
                case 3: return XtSmGraphics::Item::ReGISGraphicsGeometry;
                default: return nullopt;
            }
        }();
        if (!item.has_value())
            return ApplyResult::Invalid;

        auto const action = [&]() -> optional<XtSmGraphics::Action> {
            switch (Pa) {
                case 1: return XtSmGraphics::Action::Read;
                case 2: return XtSmGraphics::Action::ResetToDefault;
                case 3: return XtSmGraphics::Action::SetToValue;
                case 4: return XtSmGraphics::Action::ReadLimit;
                default: return nullopt;
            }
        }();
        if (!action.has_value())
            return ApplyResult::Invalid;

        if (*item != XtSmGraphics::Item::NumberOfColorRegisters
                && *action == XtSmGraphics::Action::SetToValue
                && (!Pv || !Pu))
            return ApplyResult::Invalid;

        auto const value = [&]() -> XtSmGraphics::Value {
            using Action = XtSmGraphics::Action;
            switch (*action) {
                case Action::Read:
                case Action::ResetToDefault:
                case Action::ReadLimit:
                    return std::monostate{};
                case Action::SetToValue:
                    return *item == XtSmGraphics::Item::NumberOfColorRegisters
                        ? XtSmGraphics::Value{Pv}
                        : XtSmGraphics::Value{ImageSize{Width(Pv), Height(Pu)}};
            }
            return std::monostate{};
        }();

        _screen.smGraphics(*item, *action, value);

        return ApplyResult::Ok;
    }
} // }}}

Sequencer::Sequencer(Screen& _screen,
                     ImageSize _maxImageSize,
                     RGBAColor _backgroundColor,
                     shared_ptr<SixelColorPalette> _imageColorPalette) :
    screen_{ _screen },
    imageColorPalette_{ std::move(_imageColorPalette) },
    maxImageSize_{ _maxImageSize },
    backgroundColor_{ _backgroundColor }
{
}

void Sequencer::error(std::string_view const& _errorString)
{
    if (!VTParserLog)
        return;

    LOGSTORE(VTParserLog)("Parser error: {}", _errorString);
}

void Sequencer::print(char32_t _char)
{
    precedingGraphicCharacter_ = _char;
    instructionCounter_++;
    screen_.writeText(_char);
}

void Sequencer::print(string_view _chars)
{
    if (_chars.empty())
        return;

    precedingGraphicCharacter_ = _chars.back();
    instructionCounter_ += _chars.size();
    screen_.writeText(_chars);
}

void Sequencer::execute(char _controlCode)
{
    executeControlFunction(_controlCode);
}

void Sequencer::clear()
{
    sequence_.clear();
}

void Sequencer::collect(char _char)
{
    sequence_.intermediateCharacters().push_back(_char);
}

void Sequencer::collectLeader(char _leader)
{
    sequence_.setLeader(_leader);
}

void Sequencer::param(char _char)
{
    if (sequence_.parameters().empty())
        sequence_.parameters().push_back({0});

    switch (_char)
    {
        case ';':
            if (sequence_.parameters().size() < Sequence::MaxParameters)
                sequence_.parameters().push_back({0});
            break;
        case ':':
            if (sequence_.parameters().back().size() < Sequence::MaxParameters)
                sequence_.parameters().back().push_back({0});
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            sequence_.parameters().back().back() = sequence_.parameters().back().back() * 10 + (_char - U'0');
            break;
    }
}

void Sequencer::dispatchESC(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::ESC);
    sequence_.setFinalChar(_finalChar);
    handleSequence();
}

void Sequencer::dispatchCSI(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::CSI);
    sequence_.setFinalChar(_finalChar);
    handleSequence();
}

void Sequencer::startOSC()
{
    sequence_.setCategory(FunctionCategory::OSC);
}

void Sequencer::putOSC(char32_t _char)
{
    uint8_t u8[4];
    size_t const count = distance(u8, unicode::encoder<char>{}(_char, u8));
    if (sequence_.intermediateCharacters().size() + count < Sequence::MaxOscLength)
        for (size_t i = 0; i < count; ++i)
            sequence_.intermediateCharacters().push_back(u8[i]);
}

void Sequencer::dispatchOSC()
{
    auto const [code, skipCount] = parseOSC(sequence_.intermediateCharacters());
    sequence_.parameters().push_back({static_cast<Sequence::Parameter>(code)});
    sequence_.intermediateCharacters().erase(0, skipCount);
    handleSequence();
    sequence_.clear();
}

void Sequencer::hook(char _finalChar)
{
    instructionCounter_++;
    sequence_.setCategory(FunctionCategory::DCS);
    sequence_.setFinalChar(_finalChar);

#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTParserTraceLog)
        LOGSTORE(VTParserTraceLog)("Handle VT sequence: {}", sequence_);
#endif

    if (FunctionDefinition const* funcSpec = sequence_.functionDefinition(); funcSpec != nullptr)
    {
        switch (funcSpec->id())
        {
            case DECSIXEL:
                hookedParser_ = hookSixel(sequence_);
                break;
            case STP:
                hookedParser_ = hookSTP(sequence_);
                break;
            case DECRQSS:
                hookedParser_ = hookDECRQSS(sequence_);
                break;
            case XTGETTCAP:
                hookedParser_ = hookXTGETTCAP(sequence_);
                break;
        }

        if (hookedParser_)
            hookedParser_->start();
    }
}

void Sequencer::put(char32_t _char)
{
    if (hookedParser_)
        hookedParser_->pass(_char);
}

void Sequencer::unhook()
{
    if (hookedParser_)
    {
        hookedParser_->finalize();
        hookedParser_.reset();
    }
}

unique_ptr<ParserExtension> Sequencer::hookSixel(Sequence const& _seq)
{
    auto const Pa = _seq.param_or(0, 1);
    auto const Pb = _seq.param_or(1, 2);

    auto const aspectVertical = [](int Pa) {
        switch (Pa) {
            case 9:
            case 8:
            case 7:
                return 1;
            case 6:
            case 5:
                return 2;
            case 4:
            case 3:
                return 3;
            case 2:
                return 5;
            case 1:
            case 0:
            default:
                return 2;
        }
    }(Pa);

    auto const aspectHorizontal = 1;
    auto const transparentBackground = Pb == 1;

    sixelImageBuilder_ = make_unique<SixelImageBuilder>(
        maxImageSize_,
        aspectVertical,
        aspectHorizontal,
        transparentBackground
            ? RGBAColor{0, 0, 0, 0}
            : backgroundColor_,
        usePrivateColorRegisters_
            ? make_shared<SixelColorPalette>(maxImageRegisterCount_, clamp(maxImageRegisterCount_, 0u, 16384u))
            : imageColorPalette_
    );

    return make_unique<SixelParser>(
        *sixelImageBuilder_,
        [this]() {
            {
                screen_.sixelImage(
                    sixelImageBuilder_->size(),
                    move(sixelImageBuilder_->data())
                );
            }
        }
    );
}

unique_ptr<ParserExtension> Sequencer::hookSTP(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](u32string_view const& _data) {
            screen_.eventListener().setTerminalProfile(unicode::convert_to<char>(_data));
        }
    );
}

unique_ptr<ParserExtension> Sequencer::hookXTGETTCAP(Sequence const& /*_seq*/)
{
    // DCS + q Pt ST
    //           Request Termcap/Terminfo String (XTGETTCAP), xterm.  The
    //           string following the "q" is a list of names encoded in
    //           hexadecimal (2 digits per character) separated by ; which
    //           correspond to termcap or terminfo key names.
    //           A few special features are also recognized, which are not key
    //           names:
    //
    //           o   Co for termcap colors (or colors for terminfo colors), and
    //
    //           o   TN for termcap name (or name for terminfo name).
    //
    //           o   RGB for the ncurses direct-color extension.
    //               Only a terminfo name is provided, since termcap
    //               applications cannot use this information.
    //
    //           xterm responds with
    //           DCS 1 + r Pt ST for valid requests, adding to Pt an = , and
    //           the value of the corresponding string that xterm would send,
    //           or
    //           DCS 0 + r Pt ST for invalid requests.
    //           The strings are encoded in hexadecimal (2 digits per
    //           character).

    return make_unique<SimpleStringCollector>(
        [this](u32string_view const& _data) {
            auto const capsInHex = crispy::split(_data, U';');
            for (auto hexCap: capsInHex)
            {
                string const hexCap8 = unicode::convert_to<char>(hexCap);
                if (auto const capOpt = crispy::fromHexString(string_view(hexCap8.data(), hexCap8.size())))
                    screen_.requestCapability(capOpt.value());
            }
        }
    );
}

unique_ptr<ParserExtension> Sequencer::hookDECRQSS(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](u32string_view const& _data) {
            auto const s = [](u32string_view const& _dataString) -> optional<RequestStatusString> {
                auto const mappings = std::array<std::pair<u32string_view, RequestStatusString>, 9>{
                    pair{U"m",   RequestStatusString::SGR},
                    pair{U"\"p", RequestStatusString::DECSCL},
                    pair{U" q",  RequestStatusString::DECSCUSR},
                    pair{U"\"q", RequestStatusString::DECSCA},
                    pair{U"r",   RequestStatusString::DECSTBM},
                    pair{U"s",   RequestStatusString::DECSLRM},
                    pair{U"t",   RequestStatusString::DECSLPP},
                    pair{U"$|",  RequestStatusString::DECSCPP},
                    pair{U"*|",  RequestStatusString::DECSNLS}
                };
                for (auto const& mapping : mappings)
                    if (_dataString == mapping.first)
                        return mapping.second;
                return nullopt;
            }(_data);

            if (s.has_value())
                screen_.requestStatusString(s.value());

            // TODO: handle batching
        }
    );
}

void Sequencer::executeControlFunction(char _c0)
{
    instructionCounter_++;
    switch (_c0)
    {
        case 0x07: // BEL
            screen_.eventListener().bell();
            break;
        case 0x08: // BS
            screen_.backspace();
            break;
        case 0x09: // TAB
            screen_.moveCursorToNextTab();
            break;
        case 0x0A: // LF
            screen_.linefeed();
            break;
        case 0x0B: // VT
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            [[fallthrough]];
        case 0x0C: // FF
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            screen_.index();
            break;
        case 0x0D:
            screen_.moveCursorToBeginOfLine();
            break;
        case 0x37:
            screen_.saveCursor();
            break;
        case 0x38:
            screen_.restoreCursor();
            break;
        default:
            if (VTParserLog)
                LOGSTORE(VTParserLog)("Unsupported C0 sequence: {}", crispy::escape(_c0));
            break;
    }
}

void Sequencer::handleSequence()
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTParserTraceLog)
        LOGSTORE(VTParserTraceLog)("Handle VT sequence: {}", sequence_);
#endif
    // std::cerr << fmt::format("\t{} \t; {}\n", sequence_,
    //         sequence_.functionDefinition() ? sequence_.functionDefinition()->comment : ""sv);

    instructionCounter_++;
    if (FunctionDefinition const* funcSpec = sequence_.functionDefinition(); funcSpec != nullptr)
    {
        applyAndLog(*funcSpec, sequence_);
        screen_.verifyState();
    }
    else if (VTParserLog)
        LOGSTORE(VTParserLog)("Unknown VT sequence: {}", sequence_);
}

void Sequencer::applyAndLog(FunctionDefinition const& _function, Sequence const& _seq)
{
    auto const result = apply(_function, _seq);
    switch (result)
    {
        case ApplyResult::Invalid:
            LOGSTORE(VTParserLog)("Invalid VT sequence: {}", _seq);
            break;
        case ApplyResult::Unsupported:
            LOGSTORE(VTParserLog)("Unsupported VT sequence: {}", _seq);
            break;
        case ApplyResult::Ok:
            break;
    }
}

/// Applies a FunctionDefinition to a given context, emitting the respective command.
ApplyResult Sequencer::apply(FunctionDefinition const& _function, Sequence const& _seq)
{
    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionDefinition
    switch (_function)
    {
        // C0
        case BEL: screen_.eventListener().bell(); break;
        case BS: screen_.backspace(); break;
        case TAB: screen_.moveCursorToNextTab(); break;
        case LF: screen_.linefeed(); break;
        case VT: [[fallthrough]];
        case FF: screen_.index(); break;
        case CR: screen_.moveCursorToBeginOfLine(); break;

        // ESC
        case SCS_G0_SPECIAL: screen_.designateCharset(CharsetTable::G0, CharsetId::Special); break;
        case SCS_G0_USASCII: screen_.designateCharset(CharsetTable::G0, CharsetId::USASCII); break;
        case SCS_G1_SPECIAL: screen_.designateCharset(CharsetTable::G1, CharsetId::Special); break;
        case SCS_G1_USASCII: screen_.designateCharset(CharsetTable::G1, CharsetId::USASCII); break;
        case DECALN: screen_.screenAlignmentPattern(); break;
        case DECBI: screen_.backIndex(); break;
        case DECFI: screen_.forwardIndex(); break;
        case DECKPAM: screen_.applicationKeypadMode(true); break;
        case DECKPNM: screen_.applicationKeypadMode(false); break;
        case DECRS: screen_.restoreCursor(); break;
        case DECSC: screen_.saveCursor(); break;
        case HTS: screen_.horizontalTabSet(); break;
        case IND: screen_.index(); break;
        case NEL: screen_.moveCursorToNextLine(LineCount(1)); break;
        case RI: screen_.reverseIndex(); break;
        case RIS: screen_.resetHard(); break;
        case SS2: screen_.singleShiftSelect(CharsetTable::G2); break;
        case SS3: screen_.singleShiftSelect(CharsetTable::G3); break;

        // CSI
        case ANSISYSSC: screen_.restoreCursor(); break;
        case CBT: screen_.cursorBackwardTab(TabStopCount(_seq.param_or(0, Sequence::Parameter{1}))); break;
        case CHA: screen_.moveCursorToColumn(ColumnPosition(_seq.param_or(0, Sequence::Parameter{1}))); break;
        case CHT: screen_.cursorForwardTab(TabStopCount(_seq.param_or(0, Sequence::Parameter{1}))); break;
        case CNL: screen_.moveCursorToNextLine(LineCount(_seq.param_or(0, Sequence::Parameter{1}))); break;
        case CPL: screen_.moveCursorToPrevLine(LineCount(_seq.param_or(0, Sequence::Parameter{1}))); break;
        case CPR: return impl::CPR(_seq, screen_);
        case CUB: screen_.moveCursorBackward(_seq.param_or<ColumnCount>(0, ColumnCount{1})); break;
        case CUD: screen_.moveCursorDown(_seq.param_or<LineCount>(0, LineCount{1})); break;
        case CUF: screen_.moveCursorForward(_seq.param_or<ColumnCount>(0, ColumnCount{1})); break;
        case CUP: screen_.moveCursorTo(Coordinate{ _seq.param_or<int>(0, 1), _seq.param_or<int>(1, 1)}); break;
        case CUU: screen_.moveCursorUp(_seq.param_or<LineCount>(0, LineCount{1})); break;
        case DA1: screen_.sendDeviceAttributes(); break;
        case DA2: screen_.sendTerminalId(); break;
        case DA3: return ApplyResult::Unsupported;
        case DCH: screen_.deleteCharacters(_seq.param_or<ColumnCount>(0, ColumnCount{1})); break;
        case DECCRA:
            {
                // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
                // DECCRA is not affected by the page margins.
                auto const origin = screen_.origin();
                auto const top = _seq.param_or<int>(0, origin.row);
                auto const left = _seq.param_or<int>(1, origin.column);
                auto const bottom = _seq.param_or<int>(2, unbox<int>(screen_.size().lines));
                auto const right = _seq.param_or<int>(3, unbox<int>(screen_.size().columns));
                auto const page = _seq.param_or<int>(4, 0);

                auto const targetTop = _seq.param_or<int>(5, origin.row);
                auto const targetLeft = _seq.param_or<int>(6, origin.column);
                auto const targetPage = _seq.param_or<int>(7, 0);

                screen_.copyArea(top, left, bottom, right, page,
                                 targetTop, targetLeft, targetPage);
            }
            break;
        case DECERA:
            {
                // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
                auto const origin = screen_.origin();
                auto const top = _seq.param_or(0, origin.row);
                auto const left = _seq.param_or(1, origin.column);

                // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the value is treated as the width or height of that page.
                auto const size = screen_.size();
                auto const bottom = min(_seq.param_or<int>(2, unbox<int>(size.lines)), unbox<int>(size.lines));
                auto const right = min(_seq.param_or<int>(3, unbox<int>(size.columns)), unbox<int>(size.columns));

                screen_.eraseArea(top, left, bottom, right);
            }
            break;
        case DECFRA:
            {
                auto const ch = _seq.param_or(0, Sequence::Parameter{ 0 });
                // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
                auto const origin = screen_.origin();
                auto const top = _seq.param_or(0, origin.row);
                auto const left = _seq.param_or(1, origin.column);

                // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the value is treated as the width or height of that page.
                auto const size = screen_.size();
                auto const bottom = min(_seq.param_or(2, unbox<int>(size.lines)), unbox<int>(size.lines));
                auto const right = min(_seq.param_or(3, unbox<int>(size.columns)), unbox<int>(size.columns));

                screen_.fillArea(ch, top, left, bottom, right);
            }
            break;
        case DECDC: screen_.deleteColumns(_seq.param_or<ColumnCount>(0, ColumnCount(1))); break;
        case DECIC: screen_.insertColumns(_seq.param_or<ColumnCount>(0, ColumnCount(1))); break;
        case DECRM:
            {
                ApplyResult r = ApplyResult::Ok;
                crispy::for_each(crispy::times(_seq.parameterCount()), [&](size_t i) {
                    auto const t = impl::setModeDEC(_seq, i, false, screen_);
                    r = max(r, t);
                });
                return r;
            }
            break;
        case DECRQM:
            if (_seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            screen_.requestDECMode(_seq.param(0));
            return ApplyResult::Ok;
        case DECRQM_ANSI:
            if (_seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            screen_.requestAnsiMode(_seq.param(0));
            return ApplyResult::Ok;
        case DECRQPSR: return impl::DECRQPSR(_seq, screen_);
        case DECSCUSR: return impl::DECSCUSR(_seq, screen_);
        case DECSCPP:
            if (auto const columnCount = _seq.param_or(0, 80); columnCount == 80 || columnCount == 132)
            {
                // EXTENSION: only 80 and 132 are specced, but we allow any.
                screen_.resizeColumns(ColumnCount(columnCount), false);
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        case DECSNLS:
            screen_.resize(PageSize{screen_.size().lines, _seq.param<ColumnCount>(0)});
            return ApplyResult::Ok;
        case DECSLRM: screen_.setLeftRightMargin(_seq.param_opt(0), _seq.param_opt(1)); break;
        case DECSM:
            {
                ApplyResult r = ApplyResult::Ok;
                crispy::for_each(crispy::times(_seq.parameterCount()), [&](size_t i)
                {
                    auto const t = impl::setModeDEC(_seq, i, true, screen_);
                    r = max(r, t);
                });
                return r;
            }
        case DECSTBM: screen_.setTopBottomMargin(_seq.param_opt(0), _seq.param_opt(1)); break;
        case DECSTR: screen_.resetSoft(); break;
        case DECXCPR: screen_.reportExtendedCursorPosition(); break;
        case DL: screen_.deleteLines(_seq.param_or<LineCount>(0, LineCount(1))); break;
        case ECH: screen_.eraseCharacters(_seq.param_or<ColumnCount>(0, ColumnCount(1))); break;
        case ED: return impl::ED(_seq, screen_);
        case EL: return impl::EL(_seq, screen_);
        case HPA: screen_.moveCursorToColumn(_seq.param<ColumnPosition>(0)); break;
        case HPR: screen_.moveCursorForward(_seq.param<ColumnCount>(0)); break;
        case HVP:
            screen_.moveCursorTo(Coordinate{
                _seq.param_or<int>(0, 1),
                _seq.param_or<int>(1, 1)
            });
            break; // YES, it's like a CUP!
        case ICH: screen_.insertCharacters(_seq.param_or<ColumnCount>(0, ColumnCount{1})); break;
        case IL:  screen_.insertLines(_seq.param_or<LineCount>(0, LineCount{1})); break;
        case REP:
            if (precedingGraphicCharacter_)
            {
                auto const requestedCount = _seq.param<int>(0);
                auto const availableColumns = screen_.margin().horizontal.to - screen_.cursor().position.column + 1;
                auto const effectiveCount = min(requestedCount, availableColumns);
                for (int i = 0; i < effectiveCount; i++)
                    screen_.writeText(precedingGraphicCharacter_);
            }
            break;
        case RM:
            {
                ApplyResult r = ApplyResult::Ok;
                crispy::for_each(crispy::times(_seq.parameterCount()), [&](size_t i)
                {
                    auto const t = impl::setAnsiMode(_seq, i, false, screen_);
                    r = max(r, t);
                });
                return r;
            }
            break;
        case SCOSC: screen_.saveCursor(); break;
        case SD: screen_.scrollDown(_seq.param_or<LineCount>(0, LineCount{1})); break;
        case SETMARK: screen_.setMark(); break;
        case SGR: return impl::dispatchSGR(_seq, screen_);
        case SM:
            {
                ApplyResult r = ApplyResult::Ok;
                crispy::for_each(crispy::times(_seq.parameterCount()), [&](size_t i)
                {
                    auto const t = impl::setAnsiMode(_seq, i, true, screen_);
                    r = max(r, t);
                });
                return r;
            }
        case SU: screen_.scrollUp(_seq.param_or<LineCount>(0, LineCount(1))); break;
        case TBC: return impl::TBC(_seq, screen_);
        case VPA: screen_.moveCursorToLine(_seq.param_or<LinePosition>(0, LinePosition{1})); break;
        case WINMANIP: return impl::WINDOWMANIP(_seq, screen_);
        case DECMODERESTORE: return impl::restoreDECModes(_seq, screen_);
        case DECMODESAVE: return impl::saveDECModes(_seq, screen_);
        case XTSMGRAPHICS: return impl::XTSMGRAPHICS(_seq, screen_);
        case XTVERSION:
            screen_.reply(fmt::format("\033P>|{} {}\033\\",
                                      LIBTERMINAL_NAME,
                                      LIBTERMINAL_VERSION_STRING));
            return ApplyResult::Ok;

        // OSC
        case SETTITLE:
            //(not supported) ChangeIconTitle(_seq.intermediateCharacters());
            screen_.setWindowTitle(_seq.intermediateCharacters());
            return ApplyResult::Ok;
        case SETICON:
            return ApplyResult::Ok; // NB: Silently ignore!
        case SETWINTITLE: screen_.setWindowTitle(_seq.intermediateCharacters()); break;
        case SETXPROP: return ApplyResult::Unsupported;
        case SETCOLPAL: return impl::SETCOLPAL(_seq, screen_);
        case RCOLPAL: return impl::RCOLPAL(_seq, screen_);
        case SETCWD: return impl::SETCWD(_seq, screen_);
        case HYPERLINK: return impl::HYPERLINK(_seq, screen_);
        case CAPTURE: return impl::CAPTURE(_seq, screen_);
        case COLORFG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::DefaultForegroundColor);
        case COLORBG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::DefaultBackgroundColor);
        case COLORCURSOR: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::TextCursorColor);
        case COLORMOUSEFG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::MouseForegroundColor);
        case COLORMOUSEBG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::MouseBackgroundColor);
        case SETFONT: return impl::setFont(_seq, screen_);
        case SETFONTALL: return impl::setAllFont(_seq, screen_);
        case CLIPBOARD: return impl::clipboard(_seq, screen_);
        // TODO: case COLORSPECIAL: return impl::setOrRequestDynamicColor(_seq, _output, DynamicColorName::HighlightForegroundColor);
        case RCOLORFG: screen_.resetDynamicColor(DynamicColorName::DefaultForegroundColor); break;
        case RCOLORBG: screen_.resetDynamicColor(DynamicColorName::DefaultBackgroundColor); break;
        case RCOLORCURSOR: screen_.resetDynamicColor(DynamicColorName::TextCursorColor); break;
        case RCOLORMOUSEFG: screen_.resetDynamicColor(DynamicColorName::MouseForegroundColor); break;
        case RCOLORMOUSEBG: screen_.resetDynamicColor(DynamicColorName::MouseBackgroundColor); break;
        case RCOLORHIGHLIGHTFG: screen_.resetDynamicColor(DynamicColorName::HighlightForegroundColor); break;
        case RCOLORHIGHLIGHTBG: screen_.resetDynamicColor(DynamicColorName::HighlightBackgroundColor); break;
        case NOTIFY: return impl::NOTIFY(_seq, screen_);
        case DUMPSTATE: screen_.dumpState(); break;
        default: return ApplyResult::Unsupported;
    }
    return ApplyResult::Ok;
}

std::string to_string(AnsiMode _mode)
{
    switch (_mode)
    {
        case AnsiMode::KeyboardAction: return "KeyboardAction";
        case AnsiMode::Insert: return "Insert";
        case AnsiMode::SendReceive: return "SendReceive";
        case AnsiMode::AutomaticNewLine: return "AutomaticNewLine";
    }

    return fmt::format("({})", static_cast<unsigned>(_mode));
}

std::string to_string(DECMode _mode)
{
    switch (_mode)
    {
        case DECMode::UseApplicationCursorKeys: return "UseApplicationCursorKeys";
        case DECMode::DesignateCharsetUSASCII: return "DesignateCharsetUSASCII";
        case DECMode::Columns132: return "Columns132";
        case DECMode::SmoothScroll: return "SmoothScroll";
        case DECMode::ReverseVideo: return "ReverseVideo";
        case DECMode::MouseProtocolX10: return "MouseProtocolX10";
        case DECMode::MouseProtocolNormalTracking: return "MouseProtocolNormalTracking";
        case DECMode::MouseProtocolHighlightTracking: return "MouseProtocolHighlightTracking";
        case DECMode::MouseProtocolButtonTracking: return "MouseProtocolButtonTracking";
        case DECMode::MouseProtocolAnyEventTracking: return "MouseProtocolAnyEventTracking";
        case DECMode::SaveCursor: return "SaveCursor";
        case DECMode::ExtendedAltScreen: return "ExtendedAltScreen";
        case DECMode::Origin: return "Origin";
        case DECMode::AutoWrap: return "AutoWrap";
        case DECMode::PrinterExtend: return "PrinterExtend";
        case DECMode::LeftRightMargin: return "LeftRightMargin";
        case DECMode::ShowToolbar: return "ShowToolbar";
        case DECMode::BlinkingCursor: return "BlinkingCursor";
        case DECMode::VisibleCursor: return "VisibleCursor";
        case DECMode::ShowScrollbar: return "ShowScrollbar";
        case DECMode::AllowColumns80to132: return "AllowColumns80to132";
        case DECMode::DebugLogging: return "DebugLogging";
        case DECMode::UseAlternateScreen: return "UseAlternateScreen";
        case DECMode::BracketedPaste: return "BracketedPaste";
        case DECMode::FocusTracking: return "FocusTracking";
        case DECMode::SixelScrolling: return "SixelScrolling";
        case DECMode::UsePrivateColorRegisters: return "UsePrivateColorRegisters";
        case DECMode::MouseExtended: return "MouseExtended";
        case DECMode::MouseSGR: return "MouseSGR";
        case DECMode::MouseURXVT: return "MouseURXVT";
        case DECMode::MouseAlternateScroll: return "MouseAlternateScroll";
        case DECMode::BatchedRendering: return "BatchedRendering";
        case DECMode::TextReflow: return "TextReflow";
    }
    return fmt::format("({})", static_cast<unsigned>(_mode));
};

// {{{ free function helpers
CursorShape makeCursorShape(string const& _name)
{
    string const name = [](string const& _input) {
        string output;
        transform(begin(_input), end(_input), back_inserter(output), [](auto ch) { return tolower(ch); });
        return output;
    }(_name);

    if (name == "block")
        return CursorShape::Block;
    else if (name == "rectangle")
        return CursorShape::Rectangle;
    else if (name == "underscore")
        return CursorShape::Underscore;
    else if (name == "bar")
        return CursorShape::Bar;
    else
        throw runtime_error{"Invalid cursor shape."};
}
// }}}

}  // namespace terminal

