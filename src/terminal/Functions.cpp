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
#include <crispy/times.h>
#include <crispy/algorithm.h>

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

using HandlerResult = HandlerContext::Result;
using FunctionParam = HandlerContext::FunctionParam;

namespace impl // {{{ some command generator helpers
{
    HandlerResult setMode(HandlerContext& _ctx, size_t _modeIndex, bool _enable)
	{
		switch (_ctx.param(_modeIndex))
		{
			case 2:  // (AM) Keyboard Action Mode
				return HandlerResult::Unsupported;
			case 4:  // (IRM) Insert Mode
				return _ctx.emitCommand<SetMode>(Mode::Insert, _enable);
			case 12:  // (SRM) Send/Receive Mode
			case 20:  // (LNM) Automatic Newline
			default:
				return HandlerResult::Unsupported;
		}
	}

	HandlerResult setModeDEC(HandlerContext& _ctx, size_t _modeIndex, bool _enable)
	{
		switch (_ctx.param(_modeIndex))
		{
			case 1:
				return _ctx.emitCommand<SetMode>(Mode::UseApplicationCursorKeys, _enable);
			case 2:
				return _ctx.emitCommand<SetMode>(Mode::DesignateCharsetUSASCII, _enable);
			case 3:
				return _ctx.emitCommand<SetMode>(Mode::Columns132, _enable);
			case 4:
				return _ctx.emitCommand<SetMode>(Mode::SmoothScroll, _enable);
			case 5:
				return _ctx.emitCommand<SetMode>(Mode::ReverseVideo, _enable);
			case 6:
				return _ctx.emitCommand<SetMode>(Mode::Origin, _enable);
			case 7:
				return _ctx.emitCommand<SetMode>(Mode::AutoWrap, _enable);
			case 9:
				return _ctx.emitCommand<SendMouseEvents>(MouseProtocol::X10, _enable);
			case 10:
				return _ctx.emitCommand<SetMode>(Mode::ShowToolbar, _enable);
			case 12:
				return _ctx.emitCommand<SetMode>(Mode::BlinkingCursor, _enable);
			case 19:
				return _ctx.emitCommand<SetMode>(Mode::PrinterExtend, _enable);
			case 25:
				return _ctx.emitCommand<SetMode>(Mode::VisibleCursor, _enable);
			case 30:
				return _ctx.emitCommand<SetMode>(Mode::ShowScrollbar, _enable);
			case 47:
				return _ctx.emitCommand<SetMode>(Mode::UseAlternateScreen, _enable);
			case 69:
				return _ctx.emitCommand<SetMode>(Mode::LeftRightMargin, _enable);
			case 1000:
				return _ctx.emitCommand<SendMouseEvents>(MouseProtocol::NormalTracking, _enable);
			// case 1001: // TODO
			//     return _ctx.emitCommand<SendMouseEvents>(MouseProtocol::HighlightTracking, _enable);
			case 1002:
				return _ctx.emitCommand<SendMouseEvents>(MouseProtocol::ButtonTracking, _enable);
			case 1003:
				return _ctx.emitCommand<SendMouseEvents>(MouseProtocol::AnyEventTracking, _enable);
			case 1004:
				return _ctx.emitCommand<SetMode>(Mode::FocusTracking, _enable);
			case 1005:
				return _ctx.emitCommand<SetMode>(Mode::MouseExtended, _enable);
			case 1006:
				return _ctx.emitCommand<SetMode>(Mode::MouseSGR, _enable);
			case 1007:
				return _ctx.emitCommand<SetMode>(Mode::MouseAlternateScroll, _enable);
			case 1015:
				return _ctx.emitCommand<SetMode>(Mode::MouseURXVT, _enable);
			case 1047:
				return _ctx.emitCommand<SetMode>(Mode::UseAlternateScreen, _enable);
			case 1048:
				if (_enable)
					return _ctx.emitCommand<SaveCursor>();
				else
					return _ctx.emitCommand<RestoreCursor>();
			case 1049:
				if (_enable)
				{
					_ctx.emitCommand<SaveCursor>();
					_ctx.emitCommand<SetMode>(Mode::UseAlternateScreen, true);
					_ctx.emitCommand<ClearScreen>();
				}
				else
				{
					_ctx.emitCommand<SetMode>(Mode::UseAlternateScreen, false);
					_ctx.emitCommand<RestoreCursor>();
				}
				return HandlerResult::Ok;
			case 2004:
				return _ctx.emitCommand<SetMode>(Mode::BracketedPaste, _enable);
			default:
				return HandlerResult::Unsupported;
		}
	}

	/// Parses color at given parameter offset @p i and returns new offset to continue processing parameters.
	template <typename T>
	size_t parseColor(HandlerContext& _ctx, size_t i)
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
                            _ctx.emitCommand<T>(RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
                    }
                    break;
                case 3: // ":3:F:C:M:Y" (TODO)
                case 4: // ":4:F:C:M:Y:K" (TODO)
                    break;
                case 5: // ":5:P"
                    if (auto const P = _ctx.subparam(i, 1); P <= 255)
                        _ctx.emitCommand<T>(static_cast<IndexedColor>(P));
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
						_ctx.emitCommand<T>(static_cast<IndexedColor>(value));
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
						_ctx.emitCommand<T>(RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
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

	HandlerResult dispatchSGR(HandlerContext& _ctx)
	{
        if (_ctx.parameterCount() == 0)
           return _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Reset);

		for (size_t i = 0; i < _ctx.parameterCount(); ++i)
		{
			switch (_ctx.param(i))
			{
				case 0:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Reset);
                    break;
				case 1:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Bold);
					break;
				case 2:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Faint);
					break;
				case 3:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Italic);
					break;
				case 4:
                    if (_ctx.subParameterCount(i) == 1)
                    {
                        switch (_ctx.subparam(i, 0))
                        {
                            case 0: // 4:0
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoUnderline);
                                break;
                            case 1: // 4:1
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Underline);
                                break;
                            case 2: // 4:2
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::DoublyUnderlined);
                                break;
                            case 3: // 4:3
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::CurlyUnderlined);
                                break;
                            case 4: // 4:4
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::DottedUnderline);
                                break;
                            case 5: // 4:5
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::DashedUnderline);
                                break;
                            default:
                                _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Underline);
                                break;
                        }
                    }
                    else
                        _ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Underline);
					break;
				case 5:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Blinking);
					break;
				case 7:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Inverse);
					break;
				case 8:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Hidden);
					break;
				case 9:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::CrossedOut);
					break;
				case 21:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::DoublyUnderlined);
					break;
				case 22:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::Normal);
					break;
				case 23:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoItalic);
					break;
				case 24:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoUnderline);
					break;
				case 25:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoBlinking);
					break;
				case 27:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoInverse);
					break;
				case 28:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoHidden);
					break;
				case 29:
					_ctx.emitCommand<SetGraphicsRendition>(GraphicsRendition::NoCrossedOut);
					break;
				case 30:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Black);
					break;
				case 31:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Red);
					break;
				case 32:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Green);
					break;
				case 33:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Yellow);
					break;
				case 34:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Blue);
					break;
				case 35:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Magenta);
					break;
				case 36:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::Cyan);
					break;
				case 37:
					_ctx.emitCommand<SetForegroundColor>(IndexedColor::White);
					break;
				case 38:
					i = parseColor<SetForegroundColor>(_ctx, i);
					break;
				case 39:
					_ctx.emitCommand<SetForegroundColor>(DefaultColor{});
					break;
				case 40:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Black);
					break;
				case 41:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Red);
					break;
				case 42:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Green);
					break;
				case 43:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Yellow);
					break;
				case 44:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Blue);
					break;
				case 45:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Magenta);
					break;
				case 46:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::Cyan);
					break;
				case 47:
					_ctx.emitCommand<SetBackgroundColor>(IndexedColor::White);
					break;
				case 48:
					i = parseColor<SetBackgroundColor>(_ctx, i);
					break;
				case 49:
					_ctx.emitCommand<SetBackgroundColor>(DefaultColor{});
					break;
                case 58: // Reserved, but used for setting underline/decoration colors by some other VTEs (such as mintty, kitty, libvte)
					i = parseColor<SetUnderlineColor>(_ctx, i);
                    break;
				case 90:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Black);
					break;
				case 91:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Red);
					break;
				case 92:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Green);
					break;
				case 93:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Yellow);
					break;
				case 94:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Blue);
					break;
				case 95:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Magenta);
					break;
				case 96:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::Cyan);
					break;
				case 97:
					_ctx.emitCommand<SetForegroundColor>(BrightColor::White);
					break;
				case 100:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Black);
					break;
				case 101:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Red);
					break;
				case 102:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Green);
					break;
				case 103:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Yellow);
					break;
				case 104:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Blue);
					break;
				case 105:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Magenta);
					break;
				case 106:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::Cyan);
					break;
				case 107:
					_ctx.emitCommand<SetBackgroundColor>(BrightColor::White);
					break;
				default:
					// TODO: _ctx.logInvalidCSI("Invalid SGR number: {}", _ctx.param(i));;
					break;
			}
		}
		return HandlerResult::Ok;
	}

	HandlerResult requestMode(HandlerContext& /*_ctx*/, unsigned int _mode)
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
				return HandlerResult::Unsupported; // TODO
			default:
				return HandlerResult::Invalid;
		}
	}

	HandlerResult requestModeDEC(HandlerContext& /*_ctx*/, unsigned int _mode)
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
				return HandlerResult::Unsupported;
			default:
				return HandlerResult::Invalid;
		}
	}

    HandlerResult CPR(HandlerContext& _ctx)
    {
        switch (_ctx.param(0))
        {
            case 5:
                return _ctx.emitCommand<DeviceStatusReport>();
            case 6:
                return _ctx.emitCommand<ReportCursorPosition>();
            default:
                return HandlerResult::Unsupported;
        }
    }

    HandlerResult DECRQPSR(HandlerContext& _ctx)
    {
        if (_ctx.parameterCount() != 1)
            return HandlerResult::Invalid; // -> error
        else if (_ctx.param(0) == 1)
            // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
            // TODO return _ctx.emitCommand<RequestCursorState>(); // or call it with ...Detailed?
            return HandlerResult::Invalid;
        else if (_ctx.param(0) == 2)
            return _ctx.emitCommand<RequestTabStops>();
        else
            return HandlerResult::Invalid;
    }

    HandlerResult DECSCUSR(HandlerContext& _ctx)
    {
        if (_ctx.parameterCount() <= 1)
        {
            switch (_ctx.param_or(0, HandlerContext::FunctionParam{1}))
            {
                case 0:
                case 1:
                    return _ctx.emitCommand<SetCursorStyle>(CursorDisplay::Blink, CursorShape::Block);
                case 2: return _ctx.emitCommand<SetCursorStyle>(CursorDisplay::Steady, CursorShape::Block);
                case 3:
                    return _ctx.emitCommand<SetCursorStyle>(CursorDisplay::Blink, CursorShape::Underscore);
                case 4:
                    return _ctx.emitCommand<SetCursorStyle>(CursorDisplay::Steady, CursorShape::Underscore);
                case 5:
                    return _ctx.emitCommand<SetCursorStyle>(CursorDisplay::Blink, CursorShape::Bar);
                case 6:
                    return _ctx.emitCommand<SetCursorStyle>(CursorDisplay::Steady, CursorShape::Bar);
                default:
                    return HandlerResult::Invalid;
            }
        }
        else
            return HandlerResult::Invalid;
    }

    HandlerResult ED(HandlerContext& _ctx)
    {
        if (_ctx.parameterCount() == 0)
            return _ctx.emitCommand<ClearToEndOfScreen>();
        else
        {
            for (size_t i = 0; i < _ctx.parameterCount(); ++i)
            {
                switch (_ctx.param(i))
                {
                    case 0:
                        _ctx.emitCommand<ClearToEndOfScreen>();
                        break;
                    case 1:
                        _ctx.emitCommand<ClearToBeginOfScreen>();
                        break;
                    case 2:
                        _ctx.emitCommand<ClearScreen>();
                        break;
                    case 3:
                        _ctx.emitCommand<ClearScrollbackBuffer>();
                        break;
                }
            }
            return HandlerResult::Ok;
        }
    }

    HandlerResult EL(HandlerContext& _ctx)
    {
        switch (_ctx.param_or(0, FunctionParam{0}))
        {
            case 0:
                return _ctx.emitCommand<ClearToEndOfLine>();
            case 1:
                return _ctx.emitCommand<ClearToBeginOfLine>();
            case 2:
                return _ctx.emitCommand<ClearLine>();
            default:
                return HandlerResult::Invalid;
        }
    }

    HandlerResult TBC(HandlerContext& _ctx)
    {
        if (_ctx.parameterCount() != 1)
            return _ctx.emitCommand<HorizontalTabClear>(HorizontalTabClear::AllTabs);

        switch (_ctx.param(0))
        {
            case 0:
                return _ctx.emitCommand<HorizontalTabClear>(HorizontalTabClear::UnderCursor);
            case 3:
                return _ctx.emitCommand<HorizontalTabClear>(HorizontalTabClear::AllTabs);
            default:
                return HandlerResult::Invalid;
        }
    }

    HandlerResult WINDOWMANIP(HandlerContext& _ctx)
    {
        if (_ctx.parameterCount() == 3)
        {
            switch (_ctx.param(0))
            {
                case 4:
                    return _ctx.emitCommand<ResizeWindow>(_ctx.param(2), _ctx.param(1), ResizeWindow::Unit::Pixels);
                case 8:
                    return _ctx.emitCommand<ResizeWindow>(_ctx.param(2), _ctx.param(1), ResizeWindow::Unit::Characters);
                case 22:
                    return _ctx.emitCommand<SaveWindowTitle>();
                case 23:
                    return _ctx.emitCommand<RestoreWindowTitle>();
                default:
                    return HandlerResult::Unsupported;
            }
        }
        else if (_ctx.parameterCount() == 1)
        {
            switch (_ctx.param(0))
            {
                case 4:
                    // this means, resize to full display size
                    return _ctx.emitCommand<ResizeWindow>(0u, 0u, ResizeWindow::Unit::Pixels);
                case 8:
                    // i.e. full display size
                    return _ctx.emitCommand<ResizeWindow>(0u, 0u, ResizeWindow::Unit::Characters);
                default:
                    return HandlerResult::Unsupported;
            }
        }
        else
            return HandlerResult::Unsupported;
    }
} // }}}

/*constexpr*/ auto functions()
{
    auto f = array{
        // ESC
        CS_G0_SPECIAL,
        CS_G0_USASCII,
        CS_G1_SPECIAL,
        CS_G1_USASCII,
        DECALN,
        DECBI,
        DECFI,
        DECKPAM,
        DECKPNM,
        DECRS,
        DECSC,
        HTS,
        IND,
        RI,
        RIS,
        SS2,
        SS3,

        // CSI
        ANSISYSSC,
        CBT,
        CHA,
        CNL,
        CPL,
        CPR,
        CUB,
        CUD,
        CUF,
        CUP,
        CUU,
        DA1,
        DA2,
        DCH,
        DECDC,
        DECIC,
        DECRM,
        DECRQM,
        DECRQM_ANSI,
        DECRQPSR,
        DECSCUSR,
        DECSLRM,
        DECSM,
        DECSTBM,
        DECSTR,
        DECXCPR,
        DL,
        ECH,
        ED,
        EL,
        HPA,
        HPR,
        HVP,
        ICH,
        IL,
        RM,
        SCOSC,
        SD,
        SETMARK,
        SGR,
        SM,
        SU,
        TBC,
        VPA,
        WINMANIP,
    };

    // TODO: constexpr sort(Range, Pred)
    sort(f.begin(), f.end(), [](auto const& a, auto const& b) { return a < b; });
    return f;
}

FunctionSpec const* select(FunctionCategory _category,
                           char _leader,
                           unsigned _argc,
                           char _intermediate,
                           char _final)
{
    auto static const funcs = functions();

    // TODO: This is heavily inefficient. Use binary_search() instead.
    for (auto const& f: funcs)
        if (f.category == _category
                && f.leader == _leader
                && f.intermediate == _intermediate
                && f.finalSymbol == _final
                && f.minimumParameters <= _argc && _argc <= f.maximumParameters)
        return &f;

    return nullptr; // TODO
}

/// Applies a FunctionSpec to a given context, emitting the respective command.
HandlerResult apply(FunctionSpec const& _function, HandlerContext& _ctx)
{
    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionSpec
    switch (_function)
    {
        // ESC
        case CS_G0_SPECIAL: return _ctx.emitCommand<DesignateCharset>(CharsetTable::G0, Charset::Special);
        case CS_G0_USASCII: return _ctx.emitCommand<DesignateCharset>(CharsetTable::G0, Charset::USASCII);
        case CS_G1_SPECIAL: return _ctx.emitCommand<DesignateCharset>(CharsetTable::G1, Charset::Special);
        case CS_G1_USASCII: return _ctx.emitCommand<DesignateCharset>(CharsetTable::G1, Charset::USASCII);
        case DECALN: return _ctx.emitCommand<ScreenAlignmentPattern>();
        case DECBI: return _ctx.emitCommand<BackIndex>();
        case DECFI: return _ctx.emitCommand<ForwardIndex>();
        case DECKPAM: return _ctx.emitCommand<ApplicationKeypadMode>(true);
        case DECKPNM: return _ctx.emitCommand<ApplicationKeypadMode>(false);
        case DECRS: return _ctx.emitCommand<RestoreCursor>();
        case DECSC: return _ctx.emitCommand<SaveCursor>();
        case HTS: return _ctx.emitCommand<HorizontalTabSet>();
        case IND: return _ctx.emitCommand<Index>();
        case RI: return _ctx.emitCommand<ReverseIndex>();
        case RIS: return _ctx.emitCommand<FullReset>();
        case SS2: return _ctx.emitCommand<SingleShiftSelect>(CharsetTable::G2);
        case SS3: return _ctx.emitCommand<SingleShiftSelect>(CharsetTable::G3);

        // CSI
        case ANSISYSSC: return _ctx.emitCommand<RestoreCursor>();
        case CBT: return _ctx.emitCommand<CursorBackwardTab>(_ctx.param_or(0, FunctionParam{1}));
        case CHA: return _ctx.emitCommand<MoveCursorToColumn>(_ctx.param_or(0, FunctionParam{1}));
        case CNL: return _ctx.emitCommand<CursorNextLine>(_ctx.param_or(0, FunctionParam{1}));
        case CPL: return _ctx.emitCommand<CursorPreviousLine>(_ctx.param_or(0, FunctionParam{1}));
        case CPR: return impl::CPR(_ctx);
        case CUB: return _ctx.emitCommand<MoveCursorBackward>(_ctx.param_or(0, FunctionParam{0}));
        case CUD: return _ctx.emitCommand<MoveCursorDown>(_ctx.param_or(0, FunctionParam{1}));
        case CUF: return _ctx.emitCommand<MoveCursorForward>(_ctx.param_or(0, FunctionParam{1}));
        case CUP: return _ctx.emitCommand<MoveCursorTo>(_ctx.param_or(0, FunctionParam{1}), _ctx.param_or(1, FunctionParam{1}));
        case CUU: return _ctx.emitCommand<MoveCursorUp>(_ctx.param_or(0, FunctionParam{1}));
        case DA1: return _ctx.emitCommand<SendDeviceAttributes>();
        case DA2: return _ctx.emitCommand<SendTerminalId>();
        case DCH: return _ctx.emitCommand<DeleteCharacters>(_ctx.param_or(0, FunctionParam{1}));
        case DECDC: return _ctx.emitCommand<DeleteColumns>(_ctx.param_or(0, FunctionParam{1}));
        case DECIC: return _ctx.emitCommand<InsertColumns>(_ctx.param_or(0, FunctionParam{1}));
        case DECRM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setModeDEC(_ctx, i, false); }); break;
        case DECRQM: return impl::requestModeDEC(_ctx, _ctx.param(0));
        case DECRQM_ANSI: return impl::requestMode(_ctx, _ctx.param(0));
        case DECRQPSR: return impl::DECRQPSR(_ctx);
        case DECSCUSR: return impl::DECSCUSR(_ctx);
        case DECSLRM: return _ctx.emitCommand<SetLeftRightMargin>(_ctx.param_opt(0), _ctx.param_opt(1));
        case DECSM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setModeDEC(_ctx, i, true); }); break;
        case DECSTBM: return _ctx.emitCommand<SetTopBottomMargin>(_ctx.param_opt(0), _ctx.param_opt(1));
        case DECSTR: return _ctx.emitCommand<SoftTerminalReset>();
        case DECXCPR: return _ctx.emitCommand<ReportExtendedCursorPosition>();
        case DL: return _ctx.emitCommand<DeleteLines>(_ctx.param_or(0, FunctionParam{1}));
        case ECH: return _ctx.emitCommand<EraseCharacters>(_ctx.param_or(0, FunctionParam{1}));
        case ED: return impl::ED(_ctx);
        case EL: return impl::EL(_ctx);
        case HPA: return _ctx.emitCommand<HorizontalPositionAbsolute>(_ctx.param(0));
        case HPR: return _ctx.emitCommand<HorizontalPositionRelative>(_ctx.param(0));
        case HVP: return _ctx.emitCommand<MoveCursorTo>(_ctx.param_or(0, FunctionParam{1}), _ctx.param_or(1, FunctionParam{1})); // YES, it's like a CUP!
        case ICH: return _ctx.emitCommand<InsertCharacters>(_ctx.param_or(0, FunctionParam{1}));
        case IL:  return _ctx.emitCommand<InsertLines>(_ctx.param_or(0, FunctionParam{1}));
        case RM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setMode(_ctx, i, false); }); break;
        case SCOSC: return _ctx.emitCommand<SaveCursor>();
        case SD: return _ctx.emitCommand<ScrollDown>(_ctx.param_or(0, FunctionParam{1}));
        case SETMARK: return _ctx.emitCommand<SetMark>();
        case SGR: return impl::dispatchSGR(_ctx);
        case SM: for_each(times(_ctx.parameterCount()), [&](size_t i) { impl::setMode(_ctx, i, true); }); break;
        case SU: return _ctx.emitCommand<ScrollUp>(_ctx.param_or(0, FunctionParam{1}));
        case TBC: return impl::TBC(_ctx);
        case VPA: return _ctx.emitCommand<MoveCursorToLine>(_ctx.param_or(0, FunctionParam{1}));
        case WINMANIP: return impl::WINDOWMANIP(_ctx);

        // TODO: OSC

        default: return HandlerResult::Unsupported;
    }
    return HandlerResult::Ok;
}

string to_sequence(FunctionSpec const& _func, HandlerContext const& _ctx)
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
                            [](string const& x, HandlerContext::FunctionParam y) -> string {
                                return !x.empty()
                                    ? fmt::format("{}:{}", x, y)
                                    : std::to_string(y);
                            }
                        )
                    )
                : accumulate(
                        begin(p), end(p),
                        string{},
                        [](string const& x, HandlerContext::FunctionParam y) -> string {
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

} // end namespace
