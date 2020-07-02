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
#include <terminal/Functions.h>
#include <terminal/Color.h>

#include <crispy/algorithm.h>
#include <crispy/indexed.h>
#include <crispy/sort.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <array>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <string>

using crispy::times;
using crispy::for_each;

using std::accumulate;
using std::array;
using std::sort;
using std::string;
using std::string_view;
using std::stringstream;
using std::for_each;

namespace terminal {

using Parameter = Sequence::Parameter;

namespace {
    template <typename T, typename... Args>
    ApplyResult emitCommand(CommandList& _output, Args&&... args)
    {
        _output.emplace_back(T{std::forward<Args>(args)...});
        // TODO: telemetry_.increment(...);
        return ApplyResult::Ok;
    }
}

namespace impl // {{{ some command generator helpers
{
    ApplyResult setMode(Sequence const& _ctx, size_t _modeIndex, bool _enable, CommandList& _output)
	{
		switch (_ctx.param(_modeIndex))
		{
			case 2:  // (AM) Keyboard Action Mode
				return ApplyResult::Unsupported;
			case 4:  // (IRM) Insert Mode
                emitCommand<SetMode>(_output, Mode::Insert, _enable);
                return ApplyResult::Ok;
			case 12:  // (SRM) Send/Receive Mode
			case 20:  // (LNM) Automatic Newline
			default:
				return ApplyResult::Unsupported;
		}
	}

	ApplyResult setModeDEC(Sequence const& _ctx, size_t _modeIndex, bool _enable, CommandList& _output)
	{
		switch (_ctx.param(_modeIndex))
		{
			case 1: return emitCommand<SetMode>(_output, Mode::UseApplicationCursorKeys, _enable);
			case 2: return emitCommand<SetMode>(_output, Mode::DesignateCharsetUSASCII, _enable);
			case 3: return emitCommand<SetMode>(_output, Mode::Columns132, _enable);
			case 4: return emitCommand<SetMode>(_output, Mode::SmoothScroll, _enable);
			case 5: return emitCommand<SetMode>(_output, Mode::ReverseVideo, _enable);
			case 6: return emitCommand<SetMode>(_output, Mode::Origin, _enable);
			case 7: return emitCommand<SetMode>(_output, Mode::AutoWrap, _enable);
			case 9: return emitCommand<SendMouseEvents>(_output, MouseProtocol::X10, _enable);
			case 10: return emitCommand<SetMode>(_output, Mode::ShowToolbar, _enable);
			case 12: return emitCommand<SetMode>(_output, Mode::BlinkingCursor, _enable);
			case 19: return emitCommand<SetMode>(_output, Mode::PrinterExtend, _enable);
			case 25: return emitCommand<SetMode>(_output, Mode::VisibleCursor, _enable);
			case 30: return emitCommand<SetMode>(_output, Mode::ShowScrollbar, _enable);
			case 47: return emitCommand<SetMode>(_output, Mode::UseAlternateScreen, _enable);
			case 69: return emitCommand<SetMode>(_output, Mode::LeftRightMargin, _enable);
			case 1000: return emitCommand<SendMouseEvents>(_output, MouseProtocol::NormalTracking, _enable);
			// case 1001: // TODO return emitCommand<SendMouseEvents>(_output, MouseProtocol::HighlightTracking, _enable);
			case 1002: return emitCommand<SendMouseEvents>(_output, MouseProtocol::ButtonTracking, _enable);
			case 1003: return emitCommand<SendMouseEvents>(_output, MouseProtocol::AnyEventTracking, _enable);
			case 1004: return emitCommand<SetMode>(_output, Mode::FocusTracking, _enable);
			case 1005: return emitCommand<SetMode>(_output, Mode::MouseExtended, _enable);
			case 1006: return emitCommand<SetMode>(_output, Mode::MouseSGR, _enable);
			case 1007: return emitCommand<SetMode>(_output, Mode::MouseAlternateScroll, _enable);
			case 1015: return emitCommand<SetMode>(_output, Mode::MouseURXVT, _enable);
			case 1047: return emitCommand<SetMode>(_output, Mode::UseAlternateScreen, _enable);
			case 1048:
				if (_enable)
					return emitCommand<SaveCursor>(_output);
				else
					return emitCommand<RestoreCursor>(_output);
			case 1049:
				if (_enable)
				{
					emitCommand<SaveCursor>(_output);
					emitCommand<SetMode>(_output, Mode::UseAlternateScreen, true);
					emitCommand<ClearScreen>(_output);
				}
				else
				{
					emitCommand<SetMode>(_output, Mode::UseAlternateScreen, false);
					emitCommand<RestoreCursor>(_output);
				}
				return ApplyResult::Ok;
			case 2004:
				return emitCommand<SetMode>(_output, Mode::BracketedPaste, _enable);
			default:
				return ApplyResult::Unsupported;
		}
	}

	/// Parses color at given parameter offset @p i and returns new offset to continue processing parameters.
	template <typename T>
	size_t parseColor(Sequence const& _ctx, size_t i, CommandList& _output)
	{
        // We are at parameter index `i`.
        //
        // It may now follow:
        // - ":2;r;g;b"         RGB color
        // - ":3:F:C:M:Y"       CMY color  (F is scaling factor, what is max? 100 or 255?)
        // - ":4:F:C:M:Y:K"     CMYK color (F is scaling factor, what is max? 100 or 255?)
        // - ":5:P"
        // Sub-parameters can also be delimited with ';' and thus are no sub-parameters per-se.
        if (_ctx.subParameterCount(i) >= 1)
        {
            switch (_ctx.subparam(i, 0))
            {
                case 2: // ":2:R:G:B"
                    if (_ctx.subParameterCount(i) == 4)
                    {
                        auto const r = _ctx.subparam(i, 1);
                        auto const g = _ctx.subparam(i, 2);
                        auto const b = _ctx.subparam(i, 3);
                        if (r <= 255 && g <= 255 && b <= 255)
                            emitCommand<T>(_output, RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
                    }
                    break;
                case 3: // ":3:F:C:M:Y" (TODO)
                case 4: // ":4:F:C:M:Y:K" (TODO)
                    break;
                case 5: // ":5:P"
                    if (auto const P = _ctx.subparam(i, 1); P <= 255)
                        emitCommand<T>(_output, static_cast<IndexedColor>(P));
                    break;
                default:
                    break; // XXX invalid sub parameter
            }
        }

		if (i + 1 < _ctx.parameterCount())
		{
			++i;
			auto const mode = _ctx.param(i);
			if (mode == 5)
			{
				if (i + 1 < _ctx.parameterCount())
				{
					++i;
					auto const value = _ctx.param(i);
					if (i <= 255)
						emitCommand<T>(_output, static_cast<IndexedColor>(value));
					else
                        {} // TODO: _ctx.logInvalidCSI("Invalid color indexing.");
				}
				else
                    {} // TODO: _ctx.logInvalidCSI("Missing color index.");
			}
			else if (mode == 2)
			{
				if (i + 3 < _ctx.parameterCount())
				{
					auto const r = _ctx.param(i + 1);
					auto const g = _ctx.param(i + 2);
					auto const b = _ctx.param(i + 3);
					i += 3;
					if (r <= 255 && g <= 255 && b <= 255)
						emitCommand<T>(_output, RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
					else
                        {} // TODO: _ctx.logInvalidCSI("RGB color out of range.");
				}
				else
                    {} // TODO: _ctx.logInvalidCSI("Invalid color mode.");
			}
			else
                {} // TODO: _ctx.logInvalidCSI("Invalid color mode.");
		}
		else
            {} // TODO: _ctx.logInvalidCSI("Invalid color indexing.");

		return i;
	}

	ApplyResult dispatchSGR(Sequence const& _ctx, CommandList& _output)
	{
        if (_ctx.parameterCount() == 0)
           return emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Reset);

		for (size_t i = 0; i < _ctx.parameterCount(); ++i)
		{
			switch (_ctx.param(i))
			{
				case 0: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Reset); break;
				case 1: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Bold); break;
				case 2: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Faint); break;
				case 3: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Italic); break;
				case 4:
                    if (_ctx.subParameterCount(i) == 1)
                    {
                        switch (_ctx.subparam(i, 0))
                        {
                            case 0: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoUnderline); break; // 4:0
                            case 1: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Underline); break; // 4:1
                            case 2: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::DoublyUnderlined); break; // 4:2
                            case 3: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::CurlyUnderlined); break; // 4:3
                            case 4: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::DottedUnderline); break; // 4:4
                            case 5: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::DashedUnderline); break; // 4:5
                            default: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Underline); break;
                        }
                    }
                    else
                        emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Underline);
					break;
				case 5: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Blinking); break;
				case 7: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Inverse); break;
				case 8: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Hidden); break;
				case 9: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::CrossedOut); break;
				case 21: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::DoublyUnderlined); break;
				case 22: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::Normal); break;
				case 23: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoItalic); break;
				case 24: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoUnderline); break;
				case 25: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoBlinking); break;
				case 27: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoInverse); break;
				case 28: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoHidden); break;
				case 29: emitCommand<SetGraphicsRendition>(_output, GraphicsRendition::NoCrossedOut); break;
				case 30: emitCommand<SetForegroundColor>(_output, IndexedColor::Black); break;
				case 31: emitCommand<SetForegroundColor>(_output, IndexedColor::Red); break;
				case 32: emitCommand<SetForegroundColor>(_output, IndexedColor::Green); break;
				case 33: emitCommand<SetForegroundColor>(_output, IndexedColor::Yellow); break;
				case 34: emitCommand<SetForegroundColor>(_output, IndexedColor::Blue); break;
				case 35: emitCommand<SetForegroundColor>(_output, IndexedColor::Magenta); break;
				case 36: emitCommand<SetForegroundColor>(_output, IndexedColor::Cyan); break;
				case 37: emitCommand<SetForegroundColor>(_output, IndexedColor::White); break;
				case 38: i = parseColor<SetForegroundColor>(_ctx, i, _output); break;
				case 39: emitCommand<SetForegroundColor>(_output, DefaultColor{}); break;
				case 40: emitCommand<SetBackgroundColor>(_output, IndexedColor::Black); break;
				case 41: emitCommand<SetBackgroundColor>(_output, IndexedColor::Red); break;
				case 42: emitCommand<SetBackgroundColor>(_output, IndexedColor::Green); break;
				case 43: emitCommand<SetBackgroundColor>(_output, IndexedColor::Yellow); break;
				case 44: emitCommand<SetBackgroundColor>(_output, IndexedColor::Blue); break;
				case 45: emitCommand<SetBackgroundColor>(_output, IndexedColor::Magenta); break;
				case 46: emitCommand<SetBackgroundColor>(_output, IndexedColor::Cyan); break;
				case 47: emitCommand<SetBackgroundColor>(_output, IndexedColor::White); break;
				case 48: i = parseColor<SetBackgroundColor>(_ctx, i, _output); break;
				case 49: emitCommand<SetBackgroundColor>(_output, DefaultColor{}); break;
                // 58 is reserved, but used for setting underline/decoration colors by some other VTEs (such as mintty, kitty, libvte)
                case 58: i = parseColor<SetUnderlineColor>(_ctx, i, _output); break;
				case 90: emitCommand<SetForegroundColor>(_output, BrightColor::Black); break;
				case 91: emitCommand<SetForegroundColor>(_output, BrightColor::Red); break;
				case 92: emitCommand<SetForegroundColor>(_output, BrightColor::Green); break;
				case 93: emitCommand<SetForegroundColor>(_output, BrightColor::Yellow); break;
				case 94: emitCommand<SetForegroundColor>(_output, BrightColor::Blue); break;
				case 95: emitCommand<SetForegroundColor>(_output, BrightColor::Magenta); break;
				case 96: emitCommand<SetForegroundColor>(_output, BrightColor::Cyan); break;
				case 97: emitCommand<SetForegroundColor>(_output, BrightColor::White); break;
				case 100: emitCommand<SetBackgroundColor>(_output, BrightColor::Black); break;
				case 101: emitCommand<SetBackgroundColor>(_output, BrightColor::Red); break;
				case 102: emitCommand<SetBackgroundColor>(_output, BrightColor::Green); break;
				case 103: emitCommand<SetBackgroundColor>(_output, BrightColor::Yellow); break;
				case 104: emitCommand<SetBackgroundColor>(_output, BrightColor::Blue); break;
				case 105: emitCommand<SetBackgroundColor>(_output, BrightColor::Magenta); break;
				case 106: emitCommand<SetBackgroundColor>(_output, BrightColor::Cyan); break;
				case 107: emitCommand<SetBackgroundColor>(_output, BrightColor::White); break;
				default: break; // TODO: logInvalidCSI("Invalid SGR number: {}", _ctx.param(i));
			}
		}
		return ApplyResult::Ok;
	}

	ApplyResult requestMode(Sequence const& /*_ctx*/, unsigned int _mode)
	{
		switch (_mode)
		{
			case 1: // GATM, Guarded area transfer
			case 2: // KAM, Keyboard action
			case 3: // CRM, Control representation
			case 4: // IRM, Insert/replace
			case 5: // SRTM, Status reporting transfer
			case 7: // VEM, Vertical editing
			case 10: // HEM, Horizontal editing
			case 11: // PUM, Positioning unit
			case 12: // SRM, Send/receive
			case 13: // FEAM, Format effector action
			case 14: // FETM, Format effector transfer
			case 15: // MATM, Multiple area transfer
			case 16: // TTM, Transfer termination
			case 17: // SATM, Selected area transfer
			case 18: // TSM, Tabulation stop
			case 19: // EBM, Editing boundary
			case 20: // LNM, Line feed/new line
				return ApplyResult::Unsupported; // TODO
			default:
				return ApplyResult::Invalid;
		}
	}

	ApplyResult requestModeDEC(Sequence const& /*_ctx*/, unsigned int _mode)
	{
		switch (_mode)
		{
			case 1: // DECCKM, Cursor keys
			case 2: // DECANM, ANSI
			case 3: // DECCOLM, Column
			case 4: // DECSCLM, Scrolling
			case 5: // DECSCNM, Screen
			case 6: // DECOM, Origin
			case 7: // DECAWM, Autowrap
			case 8: // DECARM, Autorepeat
			case 18: // DECPFF, Print form feed
			case 19: // DECPEX, Printer extent
			case 25: // DECTCEM, Text cursor enable
			case 34: // DECRLM, Cursor direction, right to left
			case 35: // DECHEBM, Hebrew keyboard mapping
			case 36: // DECHEM, Hebrew encoding mode
			case 42: // DECNRCM, National replacement character set
			case 57: // DECNAKB, Greek keyboard mapping
			case 60: // DECHCCM*, Horizontal cursor coupling
			case 61: // DECVCCM, Vertical cursor coupling
			case 64: // DECPCCM, Page cursor coupling
			case 66: // DECNKM, Numeric keypad
			case 67: // DECBKM, Backarrow key
			case 68: // DECKBUM, Keyboard usage
			case 69: // DECVSSM / DECLRMM, Vertical split screen
			case 73: // DECXRLM, Transmit rate limiting
			case 81: // DECKPM, Key position
			case 95: // DECNCSM, No clearing screen on column change
			case 96: // DECRLCM, Cursor right to left
			case 97: // DECCRTSM, CRT save
			case 98: // DECARSM, Auto resize
			case 99: // DECMCM, Modem control
			case 100: // DECAAM, Auto answerback
			case 101: // DECCANSM, Conceal answerback message
			case 102: // DECNULM, Ignoring null
			case 103: // DECHDPXM, Half-duplex
			case 104: // DECESKM, Secondary keyboard language
			case 106: // DECOSCNM, Overscan
				return ApplyResult::Unsupported;
			default:
				return ApplyResult::Invalid;
		}
	}

    ApplyResult CPR(Sequence const& _ctx, CommandList& _output)
    {
        switch (_ctx.param(0))
        {
            case 5: return emitCommand<DeviceStatusReport>(_output);
            case 6: return emitCommand<ReportCursorPosition>(_output);
            default: return ApplyResult::Unsupported;
        }
    }

    ApplyResult DECRQPSR(Sequence const& _ctx, CommandList& _output)
    {
        if (_ctx.parameterCount() != 1)
            return ApplyResult::Invalid; // -> error
        else if (_ctx.param(0) == 1)
            // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
            // TODO return emitCommand<RequestCursorState>(); // or call it with ...Detailed?
            return ApplyResult::Invalid;
        else if (_ctx.param(0) == 2)
            return emitCommand<RequestTabStops>(_output);
        else
            return ApplyResult::Invalid;
    }

    ApplyResult DECSCUSR(Sequence const& _ctx, CommandList& _output)
    {
        if (_ctx.parameterCount() <= 1)
        {
            switch (_ctx.param_or(0, Sequence::Parameter{1}))
            {
                case 0:
                case 1: return emitCommand<SetCursorStyle>(_output, CursorDisplay::Blink, CursorShape::Block);
                case 2: return emitCommand<SetCursorStyle>(_output, CursorDisplay::Steady, CursorShape::Block);
                case 3: return emitCommand<SetCursorStyle>(_output, CursorDisplay::Blink, CursorShape::Underscore);
                case 4: return emitCommand<SetCursorStyle>(_output, CursorDisplay::Steady, CursorShape::Underscore);
                case 5: return emitCommand<SetCursorStyle>(_output, CursorDisplay::Blink, CursorShape::Bar);
                case 6: return emitCommand<SetCursorStyle>(_output, CursorDisplay::Steady, CursorShape::Bar);
                default: return ApplyResult::Invalid;
            }
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult ED(Sequence const& _ctx, CommandList& _output)
    {
        if (_ctx.parameterCount() == 0)
            return emitCommand<ClearToEndOfScreen>(_output);
        else
        {
            for (size_t i = 0; i < _ctx.parameterCount(); ++i)
            {
                switch (_ctx.param(i))
                {
                    case 0: emitCommand<ClearToEndOfScreen>(_output); break;
                    case 1: emitCommand<ClearToBeginOfScreen>(_output); break;
                    case 2: emitCommand<ClearScreen>(_output); break;
                    case 3: emitCommand<ClearScrollbackBuffer>(_output); break;
                }
            }
            return ApplyResult::Ok;
        }
    }

    ApplyResult EL(Sequence const& _ctx, CommandList& _output)
    {
        switch (_ctx.param_or(0, Parameter{0}))
        {
            case 0: return emitCommand<ClearToEndOfLine>(_output);
            case 1: return emitCommand<ClearToBeginOfLine>(_output);
            case 2: return emitCommand<ClearLine>(_output);
            default: return ApplyResult::Invalid;
        }
    }

    ApplyResult TBC(Sequence const& _ctx, CommandList& _output)
    {
        if (_ctx.parameterCount() != 1)
            return emitCommand<HorizontalTabClear>(_output, HorizontalTabClear::AllTabs);

        switch (_ctx.param(0))
        {
            case 0: return emitCommand<HorizontalTabClear>(_output, HorizontalTabClear::UnderCursor);
            case 3: return emitCommand<HorizontalTabClear>(_output, HorizontalTabClear::AllTabs);
            default: return ApplyResult::Invalid;
        }
    }

    inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(std::string_view const& s)
    {
        return crispy::splitKeyValuePairs(s, ':');
    }

    std::optional<RGBColor> parseColor(std::string_view const& _value)
    {
        try
        {
            // "rgb:RRRR/GGGG/BBBB"
            if (_value.size() == 18 && _value.substr(0, 4) == "rgb:" && _value[8] == '/' && _value[13] == '/')
            {
                auto const r = crispy::strntoul(_value.data() + 4, 4, nullptr, 16);
                auto const g = crispy::strntoul(_value.data() + 9, 4, nullptr, 16);
                auto const b = crispy::strntoul(_value.data() + 14, 4, nullptr, 16);

                return RGBColor{
                    static_cast<uint8_t>(r & 0xFF),
                    static_cast<uint8_t>(g & 0xFF),
                    static_cast<uint8_t>(b & 0xFF)
                };
            }
            return std::nullopt;
        }
        catch (...)
        {
            // that will be a formatting error in stoul() then.
            return std::nullopt;
        }
    }

    ApplyResult setOrRequestDynamicColor(Sequence const& _ctx, CommandList& _output, DynamicColorName _name)
    {
        auto const& value = _ctx.intermediateCharacters();
        if (value == "?")
            return emitCommand<RequestDynamicColor>(_output, _name);
        else if (auto color = parseColor(value); color.has_value())
            return emitCommand<SetDynamicColor>(_output, _name, color.value());
        else
            return ApplyResult::Invalid;
    }

    ApplyResult NOTIFY(Sequence const& _ctx, CommandList& _output)
    {
        auto const& value = _ctx.intermediateCharacters();
        if (auto const splits = crispy::split(value, ';'); splits.size() == 3 && splits[0] == "notify")
            return emitCommand<Notify>(_output, string(splits[1]), string(splits[2]));
        else
            return ApplyResult::Unsupported;
    }

    ApplyResult HYPERLINK(Sequence const& _ctx, CommandList& _output)
    {
        auto const& value = _ctx.intermediateCharacters();
        // hyperlink_OSC ::= OSC '8' ';' params ';' URI
        // params := pair (':' pair)*
        // pair := TEXT '=' TEXT
        if (auto const pos = value.find(';'); pos != value.npos)
        {
            auto const params = parseSubParamKeyValuePairs(value.substr(1, pos));
            auto id = string_view{};
            auto uri = string_view{};
            if (auto const p = params.find("id"); p != params.end())
                id = p->second;
            uri = value.substr(pos + 1);
            emitCommand<Hyperlink>(_output, string(id), string(uri));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult WINDOWMANIP(Sequence const& _ctx, CommandList& _output)
    {
        if (_ctx.parameterCount() == 3)
        {
            switch (_ctx.param(0))
            {
                case 4: return emitCommand<ResizeWindow>(_output, _ctx.param(2), _ctx.param(1), ResizeWindow::Unit::Pixels);
                case 8: return emitCommand<ResizeWindow>(_output, _ctx.param(2), _ctx.param(1), ResizeWindow::Unit::Characters);
                case 22: return emitCommand<SaveWindowTitle>(_output);
                case 23: return emitCommand<RestoreWindowTitle>(_output);
                default: return ApplyResult::Unsupported;
            }
        }
        else if (_ctx.parameterCount() == 1)
        {
            switch (_ctx.param(0))
            {
                case 4: return emitCommand<ResizeWindow>(_output, 0u, 0u, ResizeWindow::Unit::Pixels); // this means, resize to full display size
                case 8: return emitCommand<ResizeWindow>(_output, 0u, 0u, ResizeWindow::Unit::Characters); // i.e. full display size
                default: return ApplyResult::Unsupported;
            }
        }
        else
            return ApplyResult::Unsupported;
    }
} // }}}

FunctionSpec const* select(FunctionSelector const& _selector)
{
    auto static const funcs = functions();

    //std::cout << fmt::format("select: {}\n", _selector);

    int a = 0;
    int b = static_cast<int>(funcs.size()) - 1;
    while (a <= b)
    {
        auto const i = (a + b) / 2;
        auto const& I = funcs[i];
        auto const rel = compare(_selector, I);
        //std::cout << fmt::format(" - a:{:>2} b:{:>2} i:{} rel:{} I: {}\n", a, b, i, rel < 0 ? '<' : rel > 0 ? '>' : '=', I);
        if (rel > 0)
            a = i + 1;
        else if (rel < 0)
            b = i - 1;
        else
            return &I;
    }
    return nullptr;
}

/// Applies a FunctionSpec to a given context, emitting the respective command.
ApplyResult apply(FunctionSpec const& _function, Sequence const& _ctx, CommandList& _output)
{
    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionSpec
    switch (_function)
    {
        // ESC
        case SCS_G0_SPECIAL: return emitCommand<DesignateCharset>(_output, CharsetTable::G0, Charset::Special);
        case SCS_G0_USASCII: return emitCommand<DesignateCharset>(_output, CharsetTable::G0, Charset::USASCII);
        case SCS_G1_SPECIAL: return emitCommand<DesignateCharset>(_output, CharsetTable::G1, Charset::Special);
        case SCS_G1_USASCII: return emitCommand<DesignateCharset>(_output, CharsetTable::G1, Charset::USASCII);
        case DECALN: return emitCommand<ScreenAlignmentPattern>(_output);
        case DECBI: return emitCommand<BackIndex>(_output);
        case DECFI: return emitCommand<ForwardIndex>(_output);
        case DECKPAM: return emitCommand<ApplicationKeypadMode>(_output, true);
        case DECKPNM: return emitCommand<ApplicationKeypadMode>(_output, false);
        case DECRS: return emitCommand<RestoreCursor>(_output);
        case DECSC: return emitCommand<SaveCursor>(_output);
        case HTS: return emitCommand<HorizontalTabSet>(_output);
        case IND: return emitCommand<Index>(_output);
        case RI: return emitCommand<ReverseIndex>(_output);
        case RIS: return emitCommand<FullReset>(_output);
        case SS2: return emitCommand<SingleShiftSelect>(_output, CharsetTable::G2);
        case SS3: return emitCommand<SingleShiftSelect>(_output, CharsetTable::G3);

        // CSI
        case ANSISYSSC: return emitCommand<RestoreCursor>(_output);
        case CBT: return emitCommand<CursorBackwardTab>(_output, _ctx.param_or(0, Parameter{1}));
        case CHA: return emitCommand<MoveCursorToColumn>(_output, _ctx.param_or(0, Parameter{1}));
        case CNL: return emitCommand<CursorNextLine>(_output, _ctx.param_or(0, Parameter{1}));
        case CPL: return emitCommand<CursorPreviousLine>(_output, _ctx.param_or(0, Parameter{1}));
        case CPR: return impl::CPR(_ctx, _output);
        case CUB: return emitCommand<MoveCursorBackward>(_output, _ctx.param_or(0, Parameter{0}));
        case CUD: return emitCommand<MoveCursorDown>(_output, _ctx.param_or(0, Parameter{1}));
        case CUF: return emitCommand<MoveCursorForward>(_output, _ctx.param_or(0, Parameter{1}));
        case CUP: return emitCommand<MoveCursorTo>(_output, _ctx.param_or(0, Parameter{1}), _ctx.param_or(1, Parameter{1}));
        case CUU: return emitCommand<MoveCursorUp>(_output, _ctx.param_or(0, Parameter{1}));
        case DA1: return emitCommand<SendDeviceAttributes>(_output);
        case DA2: return emitCommand<SendTerminalId>(_output);
        case DA3: return ApplyResult::Unsupported;
        case DCH: return emitCommand<DeleteCharacters>(_output, _ctx.param_or(0, Parameter{1}));
        case DECDC: return emitCommand<DeleteColumns>(_output, _ctx.param_or(0, Parameter{1}));
        case DECIC: return emitCommand<InsertColumns>(_output, _ctx.param_or(0, Parameter{1}));
        case DECRM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setModeDEC(_ctx, i, false, _output); }); return ApplyResult::Ok;
        case DECRQM: return impl::requestModeDEC(_ctx, _ctx.param(0));
        case DECRQM_ANSI: return impl::requestMode(_ctx, _ctx.param(0));
        case DECRQPSR: return impl::DECRQPSR(_ctx, _output);
        case DECSCUSR: return impl::DECSCUSR(_ctx, _output);
        case DECSLRM: return emitCommand<SetLeftRightMargin>(_output, _ctx.param_opt(0), _ctx.param_opt(1));
        case DECSM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setModeDEC(_ctx, i, true, _output); }); return ApplyResult::Ok;
        case DECSTBM: return emitCommand<SetTopBottomMargin>(_output, _ctx.param_opt(0), _ctx.param_opt(1));
        case DECSTR: return emitCommand<SoftTerminalReset>(_output);
        case DECXCPR: return emitCommand<ReportExtendedCursorPosition>(_output);
        case DL: return emitCommand<DeleteLines>(_output, _ctx.param_or(0, Parameter{1}));
        case ECH: return emitCommand<EraseCharacters>(_output, _ctx.param_or(0, Parameter{1}));
        case ED: return impl::ED(_ctx, _output);
        case EL: return impl::EL(_ctx, _output);
        case HPA: return emitCommand<HorizontalPositionAbsolute>(_output, _ctx.param(0));
        case HPR: return emitCommand<HorizontalPositionRelative>(_output, _ctx.param(0));
        case HVP: return emitCommand<MoveCursorTo>(_output, _ctx.param_or(0, Parameter{1}), _ctx.param_or(1, Parameter{1})); // YES, it's like a CUP!
        case ICH: return emitCommand<InsertCharacters>(_output, _ctx.param_or(0, Parameter{1}));
        case IL:  return emitCommand<InsertLines>(_output, _ctx.param_or(0, Parameter{1}));
        case RM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setMode(_ctx, i, false, _output); }); return ApplyResult::Ok;
        case SCOSC: return emitCommand<SaveCursor>(_output);
        case SD: return emitCommand<ScrollDown>(_output, _ctx.param_or(0, Parameter{1}));
        case SETMARK: return emitCommand<SetMark>(_output);
        case SGR: return impl::dispatchSGR(_ctx, _output);
        case SM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setMode(_ctx, i, true, _output); }); return ApplyResult::Ok;
        case SU: return emitCommand<ScrollUp>(_output, _ctx.param_or(0, Parameter{1}));
        case TBC: return impl::TBC(_ctx, _output);
        case VPA: return emitCommand<MoveCursorToLine>(_output, _ctx.param_or(0, Parameter{1}));
        case WINMANIP: return impl::WINDOWMANIP(_ctx, _output);

        // OSC
        case SETICON: return ApplyResult::Unsupported;
        case SETWINTITLE: return emitCommand<ChangeWindowTitle>(_output, string(_ctx.intermediateCharacters()));
        case SETXPROP: return ApplyResult::Unsupported;
        case HYPERLINK: return impl::HYPERLINK(_ctx, _output);
        case COLORFG: return impl::setOrRequestDynamicColor(_ctx, _output, DynamicColorName::DefaultForegroundColor);
        case COLORBG: return impl::setOrRequestDynamicColor(_ctx, _output, DynamicColorName::DefaultBackgroundColor);
        case COLORCURSOR: return impl::setOrRequestDynamicColor(_ctx, _output, DynamicColorName::TextCursorColor);
        case COLORMOUSEFG: return impl::setOrRequestDynamicColor(_ctx, _output, DynamicColorName::MouseForegroundColor);
        case COLORMOUSEBG: return impl::setOrRequestDynamicColor(_ctx, _output, DynamicColorName::MouseBackgroundColor);
        //case COLORSPECIAL: return impl::setOrRequestDynamicColor(_ctx, _output, DynamicColorName::HighlightForegroundColor);
        case RCOLORFG: return emitCommand<ResetDynamicColor>(_output, DynamicColorName::DefaultForegroundColor);
        case RCOLORBG: return emitCommand<ResetDynamicColor>(_output, DynamicColorName::DefaultBackgroundColor);
        case RCOLORCURSOR: return emitCommand<ResetDynamicColor>(_output, DynamicColorName::TextCursorColor);
        case RCOLORMOUSEFG: return emitCommand<ResetDynamicColor>(_output, DynamicColorName::MouseForegroundColor);
        case RCOLORMOUSEBG: return emitCommand<ResetDynamicColor>(_output, DynamicColorName::MouseBackgroundColor);
        case NOTIFY: return impl::NOTIFY(_ctx, _output);

        default:
           std::cerr << "NOT FOUND: " << to_sequence(_function, _ctx) << std::endl;
           return ApplyResult::Unsupported;
    }
}

string to_sequence(FunctionSpec const& _func, Sequence const& _ctx)
{
    stringstream sstr;

    sstr << fmt::format("{}", _func.category);

    if (_func.leader)
        sstr << ' ' << _func.leader;

    sstr << ' ' << accumulate(
        begin(_ctx.parameters()),
        end(_ctx.parameters()),
        string{},
        [](string const& a, auto const& p) -> string {
            return !a.empty()
                ? fmt::format("{};{}",
                        a,
                        accumulate(
                            begin(p), end(p),
                            string{},
                            [](string const& x, Sequence::Parameter y) -> string {
                                return !x.empty()
                                    ? fmt::format("{}:{}", x, y)
                                    : std::to_string(y);
                            }
                        )
                    )
                : accumulate(
                        begin(p), end(p),
                        string{},
                        [](string const& x, Sequence::Parameter y) -> string {
                            return !x.empty()
                                ? fmt::format("{}:{}", x, y)
                                : std::to_string(y);
                        }
                    );
        }
    );

	if (_func.intermediate)
        sstr << ' ' << _func.intermediate;

    sstr << ' ' << _func.finalSymbol;

    return sstr.str();
}

string Sequence::str() const
{
    stringstream sstr;

    sstr << fmt::format("{} ", category_);

    if (leaderSymbol_)
        sstr << ' ' << leaderSymbol_;

    if (parameterCount() > 1 || (parameterCount() == 1 && parameters_[0][0] != 0))
    {
        sstr << ' ' << accumulate(
            begin(parameters_), end(parameters_), string{},
            [](string const& a, auto const& p) -> string {
                return !a.empty()
                    ? fmt::format("{};{}",
                            a,
                            accumulate(
                                begin(p), end(p),
                                string{},
                                [](string const& x, Sequence::Parameter y) -> string {
                                    return !x.empty()
                                        ? fmt::format("{}:{}", x, y)
                                        : std::to_string(y);
                                }
                            )
                        )
                    : accumulate(
                            begin(p), end(p),
                            string{},
                            [](string const& x, Sequence::Parameter y) -> string {
                                return !x.empty()
                                    ? fmt::format("{}:{}", x, y)
                                    : std::to_string(y);
                            }
                        );
            }
        );
    }

    if (!intermediateCharacters().empty())
        sstr << ' ' << intermediateCharacters();

    if (finalChar_)
        sstr << ' ' << finalChar_;

    return sstr.str();
}

} // end namespace
