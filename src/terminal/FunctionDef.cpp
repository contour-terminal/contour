/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/FunctionDef.h>
#include <terminal/Commands.h>

#include <algorithm>
#include <numeric>
#include <array>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

namespace terminal {

string to_sequence(FunctionDef const& _func, HandlerContext const& _ctx)
{
    stringstream sstr;

	switch (_func.type)
	{
		case FunctionType::ESC:
			sstr << "ESC";
			break;
		case FunctionType::CSI:
			sstr << "CSI";
			break;
		case FunctionType::OSC:
			sstr << "OSC";
			break;
	}

    if (_func.leaderSymbol)
        sstr << ' ' << *_func.leaderSymbol;

    sstr << ' ' << accumulate(
        begin(_ctx.parameters()), end(_ctx.parameters()), string{},
        [](auto a, auto p) { return !a.empty() ? fmt::format("{} {}", a, p) : std::to_string(p); });

	if (_func.followerSymbol)
        sstr << ' ' << *_func.followerSymbol;

    sstr << ' ' << _func.finalSymbol;

    return sstr.str();
}

namespace {
	constexpr FunctionDef ESC(
		std::optional<char> _leader, char _final, VTType _vt,
		std::string_view _mnemonic, std::string_view _comment) noexcept
	{
		return FunctionDef{
			FunctionType::ESC,
			_leader,
			std::nullopt,
			_final,
			_vt,
			_mnemonic,
			_comment
		};
	}

	constexpr FunctionDef CSI(
		std::optional<char> _leader, std::optional<char> _follower, char _final,
		VTType _vt, std::string_view _mnemonic, std::string_view _comment) noexcept
	{
		return FunctionDef{
			FunctionType::CSI,
			_leader, _follower, _final, _vt,
			_mnemonic, _comment
		};
	}

	// constexpr FunctionDef OSC(char _leader, std::string_view _mnemonic, std::string_view _comment) noexcept
	// {
	// 	return FunctionDef{
	// 		FunctionType::OSC,
	// 		std::nullopt,
	// 		std::nullopt,
	// 		_leader,
	// 		VTType::VT100,
	// 		_mnemonic,
	// 		_comment
	// 	};
	// }

	HandlerResult setMode(HandlerContext& _ctx, size_t _modeIndex, bool _enable)
	{
		switch (_ctx.param(_modeIndex))
		{
			case 2:  // (AM) Keyboard Action Mode
				return HandlerResult::Unsupported;
			case 4:  // (IRM) Insert Mode
				return _ctx.template emitCommand<SetMode>(Mode::Insert, _enable);
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
				return _ctx.template emitCommand<SetMode>(Mode::UseApplicationCursorKeys, _enable);
			case 2:
				return _ctx.template emitCommand<SetMode>(Mode::DesignateCharsetUSASCII, _enable);
			case 3:
				return _ctx.template emitCommand<SetMode>(Mode::Columns132, _enable);
			case 4:
				return _ctx.template emitCommand<SetMode>(Mode::SmoothScroll, _enable);
			case 5:
				return _ctx.template emitCommand<SetMode>(Mode::ReverseVideo, _enable);
			case 6:
				return _ctx.template emitCommand<SetMode>(Mode::Origin, _enable);
			case 7:
				return _ctx.template emitCommand<SetMode>(Mode::AutoWrap, _enable);
			case 9:
				return _ctx.template emitCommand<SendMouseEvents>(MouseProtocol::X10, _enable);
			case 10:
				return _ctx.template emitCommand<SetMode>(Mode::ShowToolbar, _enable);
			case 12:
				return _ctx.template emitCommand<SetMode>(Mode::BlinkingCursor, _enable);
			case 19:
				return _ctx.template emitCommand<SetMode>(Mode::PrinterExtend, _enable);
			case 25:
				return _ctx.template emitCommand<SetMode>(Mode::VisibleCursor, _enable);
			case 30:
				return _ctx.template emitCommand<SetMode>(Mode::ShowScrollbar, _enable);
			case 47:
				return _ctx.template emitCommand<SetMode>(Mode::UseAlternateScreen, _enable);
			case 69:
				return _ctx.template emitCommand<SetMode>(Mode::LeftRightMargin, _enable);
			case 1000:
				return _ctx.template emitCommand<SendMouseEvents>(MouseProtocol::NormalTracking, _enable);
			// case 1001: // TODO
			//     return _ctx.template emitCommand<SendMouseEvents>(MouseProtocol::HighlightTracking, _enable);
			case 1002:
				return _ctx.template emitCommand<SendMouseEvents>(MouseProtocol::ButtonTracking, _enable);
			case 1003:
				return _ctx.template emitCommand<SendMouseEvents>(MouseProtocol::AnyEventTracking, _enable);
			case 1004:
				return _ctx.template emitCommand<SetMode>(Mode::FocusTracking, _enable);
			case 1005:
				return _ctx.template emitCommand<SetMode>(Mode::MouseExtended, _enable);
			case 1006:
				return _ctx.template emitCommand<SetMode>(Mode::MouseSGR, _enable);
			case 1007:
				return _ctx.template emitCommand<SetMode>(Mode::MouseAlternateScroll, _enable);
			case 1015:
				return _ctx.template emitCommand<SetMode>(Mode::MouseURXVT, _enable);
			case 1047:
				return _ctx.template emitCommand<SetMode>(Mode::UseAlternateScreen, _enable);
			case 1048:
				if (_enable)
					return _ctx.template emitCommand<SaveCursor>();
				else
					return _ctx.template emitCommand<RestoreCursor>();
			case 1049:
				if (_enable)
				{
					_ctx.template emitCommand<SaveCursor>();
					_ctx.template emitCommand<SetMode>(Mode::UseAlternateScreen, true);
					_ctx.template emitCommand<ClearScreen>();
				}
				else
				{
					_ctx.template emitCommand<SetMode>(Mode::UseAlternateScreen, false);
					_ctx.template emitCommand<RestoreCursor>();
				}
				return HandlerResult::Ok;
			case 2004:
				return _ctx.template emitCommand<SetMode>(Mode::BracketedPaste, _enable);
			default:
				return HandlerResult::Unsupported;
		}
	}

	/// Parses color at given parameter offset @p i and returns new offset to continue processing parameters.
	template <typename T>
	size_t parseColor(HandlerContext& _ctx, size_t i)
	{
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
						_ctx.template emitCommand<T>(static_cast<IndexedColor>(value));
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
						_ctx.template emitCommand<T>(RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
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
		for (size_t i = 0; i < _ctx.parameterCount(); ++i)
		{
			switch (_ctx.param(i))
			{
				case 0:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Reset);
                    break;
				case 1:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Bold);
					break;
				case 2:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Faint);
					break;
				case 3:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Italic);
					break;
				case 4:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Underline);
					break;
				case 5:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Blinking);
					break;
				case 7:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Inverse);
					break;
				case 8:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Hidden);
					break;
				case 9:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::CrossedOut);
					break;
				case 21:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::DoublyUnderlined);
					break;
				case 22:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::Normal);
					break;
				case 23:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::NoItalic);
					break;
				case 24:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::NoUnderline);
					break;
				case 25:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::NoBlinking);
					break;
				case 27:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::NoInverse);
					break;
				case 28:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::NoHidden);
					break;
				case 29:
					_ctx.template emitCommand<SetGraphicsRendition>(GraphicsRendition::NoCrossedOut);
					break;
				case 30:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Black);
					break;
				case 31:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Red);
					break;
				case 32:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Green);
					break;
				case 33:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Yellow);
					break;
				case 34:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Blue);
					break;
				case 35:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Magenta);
					break;
				case 36:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::Cyan);
					break;
				case 37:
					_ctx.template emitCommand<SetForegroundColor>(IndexedColor::White);
					break;
				case 38:
					i = parseColor<SetForegroundColor>(_ctx, i);
					break;
				case 39:
					_ctx.template emitCommand<SetForegroundColor>(DefaultColor{});
					break;
				case 40:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Black);
					break;
				case 41:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Red);
					break;
				case 42:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Green);
					break;
				case 43:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Yellow);
					break;
				case 44:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Blue);
					break;
				case 45:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Magenta);
					break;
				case 46:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::Cyan);
					break;
				case 47:
					_ctx.template emitCommand<SetBackgroundColor>(IndexedColor::White);
					break;
				case 48:
					i = parseColor<SetBackgroundColor>(_ctx, i);
					break;
				case 49:
					_ctx.template emitCommand<SetBackgroundColor>(DefaultColor{});
					break;
				case 90:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Black);
					break;
				case 91:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Red);
					break;
				case 92:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Green);
					break;
				case 93:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Yellow);
					break;
				case 94:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Blue);
					break;
				case 95:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Magenta);
					break;
				case 96:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::Cyan);
					break;
				case 97:
					_ctx.template emitCommand<SetForegroundColor>(BrightColor::White);
					break;
				case 100:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Black);
					break;
				case 101:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Red);
					break;
				case 102:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Green);
					break;
				case 103:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Yellow);
					break;
				case 104:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Blue);
					break;
				case 105:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Magenta);
					break;
				case 106:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::Cyan);
					break;
				case 107:
					_ctx.template emitCommand<SetBackgroundColor>(BrightColor::White);
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
}

FunctionHandlerMap functions(VTType _vt)
{
	auto const static allFunctions = vector<pair<FunctionDef, FunctionHandler>>{
		// ESC =======================================================================================
		{
			ESC('#', '8', VTType::VT100, "DECALN", "Screen Alignment Pattern"),
			[](auto& _ctx) { return _ctx.template emitCommand<ScreenAlignmentPattern>(); }
		},
		{
			ESC(std::nullopt, '6', VTType::VT100, "DECBI", "Back Index"),
			[](auto& _ctx) { return _ctx.template emitCommand<BackIndex>(); }
		},
		{
			ESC(std::nullopt, '9', VTType::VT100, "DECFI", "Forward Index"),
			[](auto& _ctx) { return _ctx.template emitCommand<ForwardIndex>(); }
		},
		{
			ESC(std::nullopt, '=', VTType::VT100, "DECKPAM", "Keypad Application Mode"),
			[](auto& _ctx) { return _ctx.template emitCommand<ApplicationKeypadMode>(true); }
		},
		{
			ESC(std::nullopt, '>', VTType::VT100, "DECKPNM", "Keypad Numeric Mode"),
			[](auto& _ctx) { return _ctx.template emitCommand<ApplicationKeypadMode>(false); }
		},
		{
			ESC(std::nullopt, '8', VTType::VT100, "DECRS", "Restore Cursor"),
			[](auto& _ctx) { return _ctx.template emitCommand<RestoreCursor>(); }
		},
		{
			ESC(std::nullopt, '7', VTType::VT100, "DECSC", "Save Cursor"),
			[](auto& _ctx) { return _ctx.template emitCommand<SaveCursor>(); }
		},
		{
			ESC(std::nullopt, 'D', VTType::VT100, "IND", "Index"),
			[](auto& _ctx) { return _ctx.template emitCommand<Index>(); }
		},
		{
			ESC(std::nullopt, 'H', VTType::VT100, "HTS", "Horizontal Tab Set"),
			[](auto& _ctx) { return _ctx.template emitCommand<HorizontalTabSet>(); }
		},
		{
			ESC(std::nullopt, 'M', VTType::VT100, "RI", "Reverse Index"),
			[](auto& _ctx) { return _ctx.template emitCommand<ReverseIndex>(); }
		},
		{
			ESC(std::nullopt, 'c', VTType::VT100, "RIS", "Reset to Initial State (Hard Reset)"),
			[](auto& _ctx) { return _ctx.template emitCommand<FullReset>(); }
		},
		{
			ESC(std::nullopt, 'N', VTType::VT220, "SS2", "Single Shift Select (G2 Character Set)"),
			[](auto& _ctx) { return _ctx.template emitCommand<SingleShiftSelect>(CharsetTable::G2); }
		},
		{
			ESC(std::nullopt, 'O', VTType::VT220, "SS3", "Single Shift Select (G3 Character Set)"),
			[](auto& _ctx) { return _ctx.template emitCommand<SingleShiftSelect>(CharsetTable::G3); }
		},
		// CSI =======================================================================================
		{
			CSI(std::nullopt, std::nullopt, 'G', VTType::VT100, "CHA", "Move cursor to column"),
			[](auto& _ctx) { return _ctx.template emitCommand<MoveCursorToColumn>(_ctx.param_or(0, FunctionParam{1})); }
		},
		{
			CSI(std::nullopt, std::nullopt, 'E', VTType::VT100, "CNL", "Move cursor to next line"),
			[](auto& _ctx) { return _ctx.template emitCommand<CursorNextLine>(_ctx.param_or(0, FunctionParam{1})); }
		},
		{
			CSI(std::nullopt, std::nullopt, 'F', VTType::VT100, "CPL", "Move cursor to previous line"),
			[](auto& _ctx) { return _ctx.template emitCommand<CursorPreviousLine>(_ctx.param_or(0, FunctionParam{1})); }
		},
		{
			CSI(std::nullopt, std::nullopt, 'n', VTType::VT100, "CPR", "Request Cursor position"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() != 1)
					return HandlerResult::Invalid;
				else
				{
					switch (_ctx.param(0))
					{
						case 5:
							return _ctx.template emitCommand<DeviceStatusReport>();
						case 6:
							return _ctx.template emitCommand<ReportCursorPosition>();
						default:
							return HandlerResult::Unsupported;
					}
				}
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'D', VTType::VT100, "CUB", "Move cursor backward"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<MoveCursorBackward>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'B', VTType::VT100, "CUD", "Move cursor down"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<MoveCursorDown>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'C', VTType::VT100, "CUF", "Move cursor forward"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<MoveCursorForward>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'H', VTType::VT100, "CUP", "Move cursor to position"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<MoveCursorTo>(_ctx.param_or(0, FunctionParam{1}), _ctx.param_or(1, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'A', VTType::VT100, "CUU", "Move cursor up"),
			[](auto& _ctx) { return _ctx.template emitCommand<MoveCursorUp>(_ctx.param_or(0, FunctionParam{1})); }
		},
		{
			CSI(std::nullopt, std::nullopt, 'c', VTType::VT100, "DA1", "Send primary device attributes"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1 && _ctx.param_or(0, FunctionParam{0}))
					return _ctx.template emitCommand<SendDeviceAttributes>();
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI('>', std::nullopt, 'c', VTType::VT100, "DA2", "Send secondary device attributes"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1 && _ctx.param_or(0, FunctionParam{0}))
					return _ctx.template emitCommand<SendTerminalId>();
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'P', VTType::VT100, "DCH", "Delete characters"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1)
					return _ctx.template emitCommand<DeleteCharacters>(_ctx.param_or(0, FunctionParam{1}));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI('\'', std::nullopt, '~', VTType::VT420, "DECDC", "Delete column"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1)
					return _ctx.template emitCommand<DeleteColumns>(_ctx.param_or(0, FunctionParam{1}));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI('\'', std::nullopt, '}', VTType::VT420, "DECIC", "Insert column"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1)
					return _ctx.template emitCommand<InsertColumns>(_ctx.param_or(0, FunctionParam{1}));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'g', VTType::VT100, "TBC", "Horizontal Tab Clear"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() != 1)
					return _ctx.template emitCommand<HorizontalTabClear>(HorizontalTabClear::AllTabs);

                switch (_ctx.param(0))
                {
                    case 0:
                        return _ctx.template emitCommand<HorizontalTabClear>(HorizontalTabClear::UnderCursor);
                    case 3:
                        return _ctx.template emitCommand<HorizontalTabClear>(HorizontalTabClear::AllTabs);
                    default:
                        return HandlerResult::Invalid;
                }
			}
		},
		{
			CSI('?', std::nullopt, 'l', VTType::VT100, "DECRM", "Reset DEC-mode"),
			[](auto& _ctx) {
				for (size_t i = 0; i < _ctx.parameterCount(); ++i)
					setModeDEC(_ctx, i, false);
				return HandlerResult::Ok;
			}
		},
		{
			CSI('?', '$', 'p', VTType::VT100, "DECRQM", "Request DEC-mode"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() == 1)
					return requestModeDEC(_ctx, _ctx.param(0));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, '$', 'p', VTType::VT100, "DECRQM_ANSI", "Request ANSI-mode"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() == 1)
					return requestMode(_ctx, _ctx.param(0));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, '$', 'w', VTType::VT320, "DECRQPSR", "Request presentation state report"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() != 1)
					return HandlerResult::Invalid; // -> error
				else if (_ctx.param(0) == 1)
                    // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
                    // TODO return _ctx.template emitCommand<RequestCursorState>(); // or call it with ...Detailed?
					return HandlerResult::Invalid;
				else if (_ctx.param(0) == 2)
					return _ctx.template emitCommand<RequestTabStops>();
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, ' ', 'q', VTType::VT100, "DECSCUSR", "Set Cursor Style"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1)
				{
					switch (_ctx.param_or(0, FunctionParam{1}))
					{
						case 0:
						case 1:
							return _ctx.template emitCommand<SetCursorStyle>(CursorDisplay::Blink, CursorShape::Block);
						case 2:
							return _ctx.template emitCommand<SetCursorStyle>(CursorDisplay::Steady, CursorShape::Block);
						case 3:
							return _ctx.template emitCommand<SetCursorStyle>(CursorDisplay::Blink, CursorShape::Underscore);
						case 4:
							return _ctx.template emitCommand<SetCursorStyle>(CursorDisplay::Steady, CursorShape::Underscore);
                        case 5:
							return _ctx.template emitCommand<SetCursorStyle>(CursorDisplay::Blink, CursorShape::Bar);
                        case 6:
							return _ctx.template emitCommand<SetCursorStyle>(CursorDisplay::Steady, CursorShape::Bar);
						default:
							return HandlerResult::Invalid;
					}
				}
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 's', VTType::VT420, "DECSLRM", "Set left/right margin"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() != 2)
					return HandlerResult::Invalid;
				else if (false) //TODO !isEnabled(DECSLRM))
					return HandlerResult::Unsupported;
				else
				{
					auto const left = _ctx.param_opt(0);
					auto const right = _ctx.param_opt(1);
					return _ctx.template emitCommand<SetLeftRightMargin>(left, right);
				}
			}
		},
		{
			CSI('?', std::nullopt, 'h', VTType::VT100, "DECSM", "Set DEC-mode"),
			[](auto& _ctx) {
				for (size_t i = 0; i < _ctx.parameterCount(); ++i)
					setModeDEC(_ctx, i, true);
				return HandlerResult::Ok;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'r', VTType::VT100, "DECSTBM", "Set top/bottom margin"),
			[](auto& _ctx) {
				auto const top = _ctx.param_opt(0);
				auto const bottom = _ctx.param_opt(1);
				return _ctx.template emitCommand<SetTopBottomMargin>(top, bottom);
			}
		},
		{
			CSI('!', std::nullopt, 'p', VTType::VT100, "DECSTR", "Soft terminal reset"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() == 0)
					return _ctx.template emitCommand<SoftTerminalReset>();
				else
	                return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, '6', VTType::VT100, "DECXCPR", "Request extended cursor position"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<ReportExtendedCursorPosition>();
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'M', VTType::VT100, "DL",  "Delete lines"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<DeleteLines>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'X', VTType::VT420, "ECH", "Erase characters"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<EraseCharacters>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'J', VTType::VT100, "ED",  "Erase in display"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() == 0)
					return _ctx.template emitCommand<ClearToEndOfScreen>();
				else
				{
					for (size_t i = 0; i < _ctx.parameterCount(); ++i)
					{
						switch (_ctx.param(i))
						{
							case 0:
								_ctx.template emitCommand<ClearToEndOfScreen>();
								break;
							case 1:
								_ctx.template emitCommand<ClearToBeginOfScreen>();
								break;
							case 2:
								_ctx.template emitCommand<ClearScreen>();
								break;
							case 3:
								_ctx.template emitCommand<ClearScrollbackBuffer>();
								break;
						}
					}
					return HandlerResult::Ok;
				}
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'K', VTType::VT100, "EL",  "Erase in line"),
			[](auto& _ctx) {
				switch (_ctx.param_or(0, FunctionParam{0}))
				{
					case 0:
						return _ctx.template emitCommand<ClearToEndOfLine>();
					case 1:
						return _ctx.template emitCommand<ClearToBeginOfLine>();
					case 2:
						return _ctx.template emitCommand<ClearLine>();
					default:
						return HandlerResult::Invalid;
				}
			}
		},
		{
			CSI(std::nullopt, std::nullopt, '`', VTType::VT100, "HPA", "Horizontal position absolute"),
			[](auto& _ctx)
			{
				if (_ctx.parameterCount() == 1)
					return _ctx.template emitCommand<HorizontalPositionAbsolute>(_ctx.param(0));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'a', VTType::VT100, "HPR", "Horizontal position relative"),
			[](auto& _ctx)
			{
				if (_ctx.parameterCount() == 1)
					return _ctx.template emitCommand<HorizontalPositionRelative>(_ctx.param(0));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'f', VTType::VT100, "HVP", "Horizontal and vertical position"),
			[](auto& _ctx)
			{
				// deprecated, use equivalent CUP instead.
				return _ctx.template emitCommand<MoveCursorTo>(_ctx.param_or(0, FunctionParam{1}), _ctx.param_or(1, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, '@', VTType::VT420, "ICH", "Insert character"),
			[](auto& _ctx)
			{
				if (_ctx.parameterCount() <= 1)
                	return _ctx.template emitCommand<InsertCharacters>(_ctx.param_or(0, FunctionParam{1}));
            	else
	                return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'L', VTType::VT100, "IL",  "Insert lines"),
			[](auto& _ctx)
			{
				if (_ctx.parameterCount() <= 1)
					return _ctx.template emitCommand<InsertLines>(_ctx.param_or(0, FunctionParam{1}));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'l', VTType::VT100, "RM",  "Reset mode"),
			[](auto& _ctx) {
				for (size_t i = 0; i < _ctx.parameterCount(); ++i)
					setMode(_ctx, i, false);
				return HandlerResult::Ok;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'T', VTType::VT100, "SD",  "Scroll down (pan up)"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<ScrollDown>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'm', VTType::VT100, "SGR", "Select graphics rendition"),
			&dispatchSGR
		},
		{
			CSI(std::nullopt, std::nullopt, 'h', VTType::VT100, "SM",  "Set mode"),
			[](auto& _ctx) {
				for (size_t i = 0; i < _ctx.parameterCount(); ++i)
					setMode(_ctx, i, true);
				return HandlerResult::Ok;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'S', VTType::VT100, "SU",  "Scroll up (pan down)"),
			[](auto& _ctx) {
				return _ctx.template emitCommand<ScrollUp>(_ctx.param_or(0, FunctionParam{1}));
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'd', VTType::VT100, "VPA", "Vertical Position Absolute"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() <= 1)
					return _ctx.template emitCommand<MoveCursorToLine>(_ctx.param_or(0, FunctionParam{1}));
				else
					return HandlerResult::Invalid;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 't', VTType::VT525, "WINMANIP", "Window Manipulation"),
			[](auto& _ctx) {
				if (_ctx.parameterCount() == 3)
				{
					switch (_ctx.param(0))
					{
						case 4:
							return _ctx.template emitCommand<ResizeWindow>(_ctx.param(2), _ctx.param(1), ResizeWindow::Unit::Pixels);
						case 8:
							return _ctx.template emitCommand<ResizeWindow>(_ctx.param(2), _ctx.param(1), ResizeWindow::Unit::Characters);
						case 22:
							return _ctx.template emitCommand<SaveWindowTitle>();
						case 23:
							return _ctx.template emitCommand<RestoreWindowTitle>();
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
							return _ctx.template emitCommand<ResizeWindow>(0u, 0u, ResizeWindow::Unit::Pixels);
						case 8:
							// i.e. full display size
							return _ctx.template emitCommand<ResizeWindow>(0u, 0u, ResizeWindow::Unit::Characters);
						default:
							return HandlerResult::Unsupported;
					}
				}
				else
					return HandlerResult::Unsupported;
			}
		},
		{
			CSI(std::nullopt, std::nullopt, 'Z', VTType::VT100, "CBT", "Cursor Backward Tabulation"),
			[](auto& _ctx) {
                if (_ctx.parameterCount() <= 1)
                    return _ctx.template emitCommand<CursorBackwardTab>(_ctx.param_or(0, FunctionParam{1}));
                else
					return HandlerResult::Invalid;
            }
        },
		{
			CSI('>', std::nullopt, 'M', VTType::VT100, "SETMARK", "Set Vertical Mark"),
			[](auto& _ctx) { return _ctx.template emitCommand<SetMark>(); }
		}
	};

	FunctionHandlerMap result;
	for (pair<FunctionDef, FunctionHandler> const& func: allFunctions)
		if (func.first.conformanceLevel <= _vt)
			result[func.first.id()] = func;

	return result;
}

} // namespace terminal
