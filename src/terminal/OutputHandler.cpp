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
#include <terminal/OutputHandler.h>
#include <terminal/FunctionDef.h>

#include <terminal/Commands.h>
#include <terminal/Util.h>

#include <fmt/format.h>

#include <numeric>
#include <optional>
#include <sstream>

namespace terminal {

using namespace std;

optional<CharsetTable> getCharsetTableForCode(std::string const& _intermediate)
{
    if (_intermediate.size() != 1)
        return nullopt;

    char32_t const code = _intermediate[0];
    switch (code)
    {
        case '(':
            return {CharsetTable::G0};
        case ')':
        case '-':
            return {CharsetTable::G1};
        case '*':
        case '.':
            return {CharsetTable::G2};
        case '+':
        case '/':
            return {CharsetTable::G3};
        default:
            return nullopt;
    }
}

OutputHandler::OutputHandler(unsigned int _rows, Logger _logger) :
	rowCount_{_rows},
	logger_{std::move(_logger)},
	functionMapper_{functions(VTType::VT525)}
{
	parameters_.reserve(MaxParameters);
}

string OutputHandler::sequenceString(string const& _prefix) const
{
    stringstream sstr;
    sstr << _prefix;
    if (leaderSymbol_)
        sstr << ' ' << leaderSymbol_;

    sstr << ' ' << accumulate(
        begin(parameters_), end(parameters_), string{},
        [](auto a, auto p) { return !a.empty() ? fmt::format("{} {}", a, p) : std::to_string(p); });

    if (!intermediateCharacters_.empty())
        sstr << ' ' << intermediateCharacters_;

    sstr << ' ' << static_cast<char>(currentChar());

    return sstr.str();
}

void OutputHandler::invokeAction(ActionClass actionClass, Action action, char32_t _currentChar)
{
    currentChar_ = _currentChar;

    switch (action)
    {
        case Action::Clear:
			leaderSymbol_ = 0;
            intermediateCharacters_.clear();
            parameters_.resize(1);
            parameters_[0] = 0;
            defaultParameter_ = 0;
            private_ = false;
            return;
		case Action::CollectLeader:
			leaderSymbol_ = static_cast<char>(currentChar());
			return;
        case Action::Collect:
            intermediateCharacters_.push_back(static_cast<char>(currentChar())); // cast OK, because non-ASCII wouldn't be valid collected chars
            return;
        case Action::Print:
            emit<AppendChar>(currentChar());
            return;
        case Action::Param:
            if (currentChar() == ';')
                parameters_.push_back(0);
            else
                parameters_.back() = parameters_.back() * 10 + (currentChar() - U'0');
            return;
        case Action::CSI_Dispatch:
			dispatchCSI();
            return;
        case Action::Execute:
            executeControlFunction();
            return;
        case Action::ESC_Dispatch:
            if (intermediateCharacters_.empty())
                dispatchESC();
            else if (intermediateCharacters_ == "#" && currentChar() == '8')
                emit<ScreenAlignmentPattern>();
            else if (intermediateCharacters_ == "(" && currentChar_ == 'B')
                logUnsupported("Designate Character Set US-ASCII.");
            else if (currentChar_ == '0')
            {
                if (auto g = getCharsetTableForCode(intermediateCharacters_); g.has_value())
                    emit<DesignateCharset>(*g, Charset::Special);
                else
                    logInvalidESC(fmt::format("Invalid charset table identifier: {}", escape(intermediateCharacters_[0])));
            }
            else
                logInvalidESC();
            return;
        case Action::OSC_Start:
            // no need, we inline OSC_Put and OSC_End's actions
            break;
        case Action::OSC_Put:
            intermediateCharacters_.push_back(static_cast<char>(currentChar())); // cast OK, becuase only ASCII's allowed (I think) TODO: check that fact
            break;
        case Action::OSC_End:
            if (intermediateCharacters_.size() > 1 && intermediateCharacters_[1] == ';')
            {
                string const value = intermediateCharacters_.substr(2);
                switch (intermediateCharacters_[0])
                {
                    case '0':
                        emit<ChangeWindowTitle>(value);
                        break;
                    case '2':
                        emit<ChangeWindowTitle>(value);
                        break;
                    case '1': // change X11 resource
                    case '3': // change icon name (also X11 specific)
                        log<UnsupportedOutputEvent>(sequenceString("OSC"));
                        break;
                    default:
                    {
                        log<InvalidOutputEvent>(sequenceString("OSC"));
                        break;
                    }
                }
            }
            else
            {
                log<UnsupportedOutputEvent>(sequenceString("OSC"));
            }
            intermediateCharacters_.clear();
            break;
        case Action::Hook:
        case Action::Put:
        case Action::Unhook:
            logUnsupported("Action: {} {} \"{}\"", to_string(action), escape(currentChar()),
                           escape(intermediateCharacters_));
            return;
        case Action::Ignore:
        case Action::Undefined:
            return;
    }
}

void OutputHandler::executeControlFunction()
{
    switch (currentChar())
    {
        case 0x07: // BEL
            emit<Bell>();
            break;
        case 0x08: // BS
            emit<Backspace>();
            break;
        case 0x09: // TAB
            emit<MoveCursorToNextTab>();
            break;
        case 0x0A: // LF
            emit<Linefeed>();
            break;
        case 0x0B: // VT
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            emit<Index>();
            break;
        case 0x0C: // FF
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            emit<Index>();
            break;
        case 0x0D:
            emit<MoveCursorToBeginOfLine>();
            break;
        case 0x37:
            emit<SaveCursor>();
            break;
        case 0x38:
            emit<RestoreCursor>();
            break;
        default:
            logUnsupported("ESC C0 or C1 control function: {}", escape(currentChar()));
            break;
    }
}

void OutputHandler::dispatchESC()
{
    switch (currentChar())
    {
        case '6':
            emit<BackIndex>();
            break;
        case '7':
            emit<SaveCursor>();
            break;
        case '8':
            emit<RestoreCursor>();
            break;
        case '9':
            emit<ForwardIndex>();
            break;
        case '=':
            emit<ApplicationKeypadMode>(true);
            break;
        case '>':
            emit<ApplicationKeypadMode>(false);
            break;
        case 'D':
            emit<Index>();
            break;
        case 'M':
            emit<ReverseIndex>();
            break;
        case 'N':  // SS2: Single Shift Select of G2 Character Set
            emit<SingleShiftSelect>(CharsetTable::G2);
            break;
        case 'O':  // SS3: Single Shift Select of G3 Character Set
            emit<SingleShiftSelect>(CharsetTable::G3);
            break;
        case 'A':
            emit<DesignateCharset>(CharsetTable::G0, Charset::UK);
            break;
        case 'B':
            emit<DesignateCharset>(CharsetTable::G0, Charset::USASCII);
            break;
        case 'K':
            emit<DesignateCharset>(CharsetTable::G0, Charset::German);
            break;
        case 'c':
            emit<FullReset>();
            break;
        default:
            logUnsupported("ESC_Dispatch: '{}' {}", escape(currentChar()), escape(intermediateCharacters_));
            break;
    }
}

void OutputHandler::dispatchCSI()
{
    // logDebug("dispatch CSI: {} {} {}", intermediateCharacters_,
    //     accumulate(
    //         begin(parameters_), end(parameters_), string{},
    //         [](auto a, auto p) { return !a.empty() ? fmt::format("{}, {}", a, p) : std::to_string(p); }),
    //     static_cast<char>(currentChar()));

    char const followerSym = intermediateCharacters_.size() == 1
		? intermediateCharacters_[0]
		: 0;

	auto const funcId = FunctionDef::makeId(FunctionType::CSI, leaderSymbol_, followerSym, static_cast<char>(currentChar()));
#if 1
	if (auto const funcMap = functionMapper_.find(funcId); funcMap != end(functionMapper_))
	{
		auto const result = funcMap->second.second(HandlerContext{parameters_, intermediateCharacters_, commands_});
		switch (result)
		{
			case HandlerResult::Unsupported:
				logUnsupportedCSI();
				break;
			case HandlerResult::Invalid:
				logInvalidCSI();
				break;
			case HandlerResult::Ok:
				break;
		}
	}
#else
    switch (funcId)
    {
        case HVP:
            // deprecated, use CUP instead.
            [[fallthrough]];
        case CUP:
            setDefaultParameter(1);
            emit<MoveCursorTo>(param(0), param(1));
			break;
        case CUU:
            setDefaultParameter(1);
            emit<MoveCursorUp>(param(0));
			break;
        case CUD:
            setDefaultParameter(1);
            emit<MoveCursorDown>(param(0));
			break;
        case CUF:
            setDefaultParameter(1);
            emit<MoveCursorForward>(param(0));
            break;
        case CUB:
            setDefaultParameter(1);
            emit<MoveCursorBackward>(param(0));
            break;
        case CPL:
            setDefaultParameter(1);
            emit<CursorPreviousLine>(param(0));
            break;
        case CHA:
            setDefaultParameter(1);
            emit<MoveCursorToColumn>(param(0));
            break;
		case ED:
			if (parameterCount() == 0)
				emit<ClearToEndOfScreen>();
			else
			{
				for (size_t i = 0; i < parameterCount(); ++i)
				{
					switch (param(i))
					{
						case 0:
							emit<ClearToEndOfScreen>();
							break;
						case 1:
							emit<ClearToBeginOfScreen>();
							break;
						case 2:
							emit<ClearScreen>();
							break;
						case 3:
							emit<ClearScrollbackBuffer>();
							break;
					}
				}
			}
			break;
        case EL:
            setDefaultParameter(0);
            switch (param(0))
            {
                case 0:
                    emit<ClearToEndOfLine>();
                    break;
                case 1:
                    emit<ClearToBeginOfLine>();
                    break;
                case 2:
                    emit<ClearLine>();
                    break;
                default:
                    logInvalidCSI();
                    break;
            }
            break;
        case IL:
            setDefaultParameter(1);
            emit<InsertLines>(param(0));
            break;
        case DL:
            setDefaultParameter(1);
            emit<DeleteLines>(param(0));
            break;
        case DCH:
            setDefaultParameter(1);
            emit<DeleteCharacters>(param(0));
            break;
        case SU:
            setDefaultParameter(1);
            emit<ScrollUp>(param(0));
            break;
        case SD:
            setDefaultParameter(1);
            emit<ScrollDown>(param(0));
            break;
        case ECH:
            setDefaultParameter(1);
            emit<EraseCharacters>(param(0));
            break;
        case DA1:
            // Send Primary DA
            if (param(0) == 0)
                emit<SendDeviceAttributes>();
            else
                logInvalidCSI();
            break;
        case VPA:
            setDefaultParameter(1);
            emit<MoveCursorToLine>(param(0));
            break;
        case CPR:
            switch (param(0))
            {
                case 5:
                    emit<DeviceStatusReport>();
                    break;
                case 6:
                    emit<ReportCursorPosition>();
                    break;
                default:
                    logUnsupportedCSI();
                    break;
            }
            break;
		case DECRQM:
            if (parameterCount() == 1)
                requestMode(param(0));
            else
                logInvalidCSI();
            break;
		case DECSCUSR:
			setDefaultParameter(1);
			if (parameterCount() <= 1)
			{
				switch (param(0))
				{
					case 0:
					case 1:
						emit<SetCursorStyle>(CursorDisplay::Blink, CursorStyle::Block);
						break;
					case 2:
						emit<SetCursorStyle>(CursorDisplay::Steady, CursorStyle::Block);
						break;
					case 3:
						emit<SetCursorStyle>(CursorDisplay::Blink, CursorStyle::Underline);
						break;
					case 4:
						emit<SetCursorStyle>(CursorDisplay::Steady, CursorStyle::Underline);
						break;
					default:
						logInvalidCSI();
						break;
				}
			}
			else
				logInvalidCSI();
			break;
		case DECRQM_ANSI:
			if (parameterCount() == 1)
				requestMode(param(0));
			else
				logInvalidCSI();
			break;
        case DECSTBM:
        {
            setDefaultParameter(1);
            auto const top = param(0);

            setDefaultParameter(rowCount_);
            auto const bottom = param(1);

            emit<SetTopBottomMargin>(top, bottom);
            break;
        }
        case DECSLRM:
        {
            if (parameterCount() != 2)
                logInvalidCSI();
            else if (false) //TODO !isEnabled(DECSLRM))
                logUnsupportedCSI();
            else
            {
                setDefaultParameter(1);
                auto const left = param(0);
                auto const right = param(1);
                emit<SetLeftRightMargin>(left, right);
            }
            break;
        }
		case SM:
            for (size_t i = 0; i < parameterCount(); ++i)
                setMode(param(i), true);
            break;
		case DECSM:
            for (size_t i = 0; i < parameterCount(); ++i)
                setModeDEC(param(i), true);
            break;
		case RM:
            for (size_t i = 0; i < parameterCount(); ++i)
                setMode(param(i), false);
            break;
		case DECRM:
            for (size_t i = 0; i < parameterCount(); ++i)
                setModeDEC(param(i), false);
            break;
		case SGR:
            dispatchGraphicsRendition();
            break;
        case ICH:
            setDefaultParameter(1);
            if (parameterCount() == 1)
                emit<InsertCharacters>(param(0));
            else
                logInvalidCSI();
            break;
		case HPA:
            if (parameterCount() == 1)
                emit<HorizontalPositionAbsolute>(param(0));
            else
                logInvalidCSI();
            break;
		case HPR:
            if (parameterCount() == 1)
                emit<HorizontalPositionRelative>(param(0));
            else
                logInvalidCSI();
            break;
		case DECXCPR:
            emit<ReportExtendedCursorPosition>();
            break;
		case DECSTR:
            if (parameterCount() == 0)
                emit<SoftTerminalReset>();
            else
                logInvalidCSI();
            break;
		case DA2:
            // Send Secondary DA
            if (param(0) == 0)
                emit<SendTerminalId>();
            else
                logInvalidCSI();
            break;
		case DECDC:
            if (parameterCount() <= 1)
            {
                setDefaultParameter(1);
                emit<DeleteColumns>(param(0));
            }
            else
                logInvalidCSI();
            break;
		case DECIC:
            if (parameterCount() <= 1)
            {
                setDefaultParameter(1);
                emit<InsertColumns>(param(0));
            }
            else
                logInvalidCSI();
            break;
        case WINMANIP:
            setDefaultParameter(0);
            if (parameterCount() == 3)
            {
                switch (param(0))
                {
                    case 4:
                        emit<ResizeWindow>(param(2), param(1), ResizeWindow::Unit::Pixels);
                        break;
                    case 8:
                        emit<ResizeWindow>(param(2), param(1), ResizeWindow::Unit::Characters);
                        break;
                    case 22:
                        emit<SaveWindowTitle>();
                        break;
                    case 23:
                        emit<RestoreWindowTitle>();
                        break;
                    default:
                        logUnsupportedCSI();
                        break;
                }
            }
            else if (parameterCount() == 1)
            {
                switch (param(0))
                {
                    case 4:
                        // this means, resize to full display size
                        emit<ResizeWindow>(0u, 0u, ResizeWindow::Unit::Pixels);
                        break;
                    case 8:
                        // i.e. full display size
                        emit<ResizeWindow>(0u, 0u, ResizeWindow::Unit::Characters);
                        break;
                    default:
                        logUnsupportedCSI();
                        break;
                }
            }
            else
                logInvalidCSI();
            break;
        default:
            logUnsupportedCSI();
            break;
    }
#endif
}

void OutputHandler::setMode(unsigned int mode, bool enable)
{
    switch (mode)
    {
        case 2:  // (AM) Keyboard Action Mode
            logUnsupported("set-mode: {}", param(0));
            break;
        case 4:  // (IRM) Insert Mode
            emit<SetMode>(Mode::Insert, enable);
            break;
        case 12:  // (SRM) Send/Receive Mode
        case 20:  // (LNM) Automatic Newline
        default:
            logUnsupported("set-mode: {}", param(0));
            break;
    }
}

void OutputHandler::setModeDEC(unsigned int mode, bool enable)
{
    switch (mode)
    {
        case 1:
            emit<SetMode>(Mode::UseApplicationCursorKeys, enable);
            break;
        case 2:
            emit<SetMode>(Mode::DesignateCharsetUSASCII, enable);
            break;
        case 3:
            emit<SetMode>(Mode::Columns132, enable);
            break;
        case 4:
            emit<SetMode>(Mode::SmoothScroll, enable);
            break;
        case 5:
            emit<SetMode>(Mode::ReverseVideo, enable);
            break;
        case 6:
            emit<SetMode>(Mode::Origin, enable);
            break;
        case 7:
            emit<SetMode>(Mode::AutoWrap, enable);
            break;
        case 9:
            emit<SendMouseEvents>(MouseProtocol::X10, enable);
            break;
        case 10:
            emit<SetMode>(Mode::ShowToolbar, enable);
            break;
        case 12:
            emit<SetMode>(Mode::BlinkingCursor, enable);
            break;
        case 19:
            emit<SetMode>(Mode::PrinterExtend, enable);
            break;
        case 25:
            emit<SetMode>(Mode::VisibleCursor, enable);
            break;
        case 30:
            emit<SetMode>(Mode::ShowScrollbar, enable);
            break;
        case 47:
            emit<SetMode>(Mode::UseAlternateScreen, enable);
            break;
        case 69:
            emit<SetMode>(Mode::LeftRightMargin, enable);
            break;
        case 1000:
            emit<SendMouseEvents>(MouseProtocol::VT200, enable);
            break;
        case 1001:
            emit<SendMouseEvents>(MouseProtocol::VT200_Highlight, enable);
            break;
        case 1002:
            emit<SendMouseEvents>(MouseProtocol::ButtonEvent, enable);
            break;
        case 1003:
            emit<SendMouseEvents>(MouseProtocol::AnyEvent, enable);
            break;
        case 1004:
            emit<SendMouseEvents>(MouseProtocol::FocusEvent, enable);
            break;
        case 1005:
            emit<SendMouseEvents>(MouseProtocol::Extended, enable);
            break;
        case 1006:
            emit<SendMouseEvents>(MouseProtocol::SGR, enable);
            break;
        case 1007:
            emit<SendMouseEvents>(MouseProtocol::AlternateScroll, enable);
            break;
        case 1015:
            emit<SendMouseEvents>(MouseProtocol::URXVT, enable);
            break;
        case 1047:
            emit<SetMode>(Mode::UseAlternateScreen, enable);
            break;
        case 1048:
            if (enable)
                emit<SaveCursor>();
            else
                emit<RestoreCursor>();
            break;
        case 1049:
            if (enable)
            {
                emit<SaveCursor>();
                emit<SetMode>(Mode::UseAlternateScreen, true);
                emit<ClearScreen>();
            }
            else
            {
                emit<SetMode>(Mode::UseAlternateScreen, false);
                emit<RestoreCursor>();
            }
            break;
        case 2004:
            emit<SetMode>(Mode::BracketedPaste, enable);
            break;
        default:
            logUnsupported("set-mode (DEC) {}", param(0));
            break;
    }
}

void OutputHandler::requestMode(unsigned int _mode)
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
            logUnsupportedCSI();
            break;
        default:
            logInvalidCSI();
            break;
    }
}

void OutputHandler::requestModeDEC(unsigned int _mode)
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
            logUnsupportedCSI();
            break;
        default:
            logInvalidCSI();
    }
}

void OutputHandler::dispatchGraphicsRendition()
{
    for (size_t i = 0; i < parameterCount(); ++i)
    {
        switch (param(i))
        {
            case 0:
                emit<SetGraphicsRendition>(GraphicsRendition::Reset);
                break;
            case 1:
                emit<SetGraphicsRendition>(GraphicsRendition::Bold);
                break;
            case 2:
                emit<SetGraphicsRendition>(GraphicsRendition::Faint);
                break;
            case 3:
                emit<SetGraphicsRendition>(GraphicsRendition::Italic);
                break;
            case 4:
                emit<SetGraphicsRendition>(GraphicsRendition::Underline);
                break;
            case 5:
                emit<SetGraphicsRendition>(GraphicsRendition::Blinking);
                break;
            case 7:
                emit<SetGraphicsRendition>(GraphicsRendition::Inverse);
                break;
            case 8:
                emit<SetGraphicsRendition>(GraphicsRendition::Hidden);
                break;
            case 9:
                emit<SetGraphicsRendition>(GraphicsRendition::CrossedOut);
                break;
            case 21:
                emit<SetGraphicsRendition>(GraphicsRendition::DoublyUnderlined);
                break;
            case 22:
                emit<SetGraphicsRendition>(GraphicsRendition::Normal);
                break;
            case 23:
                emit<SetGraphicsRendition>(GraphicsRendition::NoItalic);
                break;
            case 24:
                emit<SetGraphicsRendition>(GraphicsRendition::NoUnderline);
                break;
            case 25:
                emit<SetGraphicsRendition>(GraphicsRendition::NoBlinking);
                break;
            case 27:
                emit<SetGraphicsRendition>(GraphicsRendition::NoInverse);
                break;
            case 28:
                emit<SetGraphicsRendition>(GraphicsRendition::NoHidden);
                break;
            case 29:
                emit<SetGraphicsRendition>(GraphicsRendition::NoCrossedOut);
                break;
            case 30:
                emit<SetForegroundColor>(IndexedColor::Black);
                break;
            case 31:
                emit<SetForegroundColor>(IndexedColor::Red);
                break;
            case 32:
                emit<SetForegroundColor>(IndexedColor::Green);
                break;
            case 33:
                emit<SetForegroundColor>(IndexedColor::Yellow);
                break;
            case 34:
                emit<SetForegroundColor>(IndexedColor::Blue);
                break;
            case 35:
                emit<SetForegroundColor>(IndexedColor::Magenta);
                break;
            case 36:
                emit<SetForegroundColor>(IndexedColor::Cyan);
                break;
            case 37:
                emit<SetForegroundColor>(IndexedColor::White);
                break;
            case 38:
                i = parseColor<SetForegroundColor>(i);
                break;
            case 39:
                emit<SetForegroundColor>(DefaultColor{});
                break;
            case 40:
                emit<SetBackgroundColor>(IndexedColor::Black);
                break;
            case 41:
                emit<SetBackgroundColor>(IndexedColor::Red);
                break;
            case 42:
                emit<SetBackgroundColor>(IndexedColor::Green);
                break;
            case 43:
                emit<SetBackgroundColor>(IndexedColor::Yellow);
                break;
            case 44:
                emit<SetBackgroundColor>(IndexedColor::Blue);
                break;
            case 45:
                emit<SetBackgroundColor>(IndexedColor::Magenta);
                break;
            case 46:
                emit<SetBackgroundColor>(IndexedColor::Cyan);
                break;
            case 47:
                emit<SetBackgroundColor>(IndexedColor::White);
                break;
            case 48:
                i = parseColor<SetBackgroundColor>(i);
                break;
            case 49:
                emit<SetBackgroundColor>(DefaultColor{});
                break;
            case 90:
                emit<SetForegroundColor>(BrightColor::Black);
                break;
            case 91:
                emit<SetForegroundColor>(BrightColor::Red);
                break;
            case 92:
                emit<SetForegroundColor>(BrightColor::Green);
                break;
            case 93:
                emit<SetForegroundColor>(BrightColor::Yellow);
                break;
            case 94:
                emit<SetForegroundColor>(BrightColor::Blue);
                break;
            case 95:
                emit<SetForegroundColor>(BrightColor::Magenta);
                break;
            case 96:
                emit<SetForegroundColor>(BrightColor::Cyan);
                break;
            case 97:
                emit<SetForegroundColor>(BrightColor::White);
                break;
            case 100:
                emit<SetBackgroundColor>(BrightColor::Black);
                break;
            case 101:
                emit<SetBackgroundColor>(BrightColor::Red);
                break;
            case 102:
                emit<SetBackgroundColor>(BrightColor::Green);
                break;
            case 103:
                emit<SetBackgroundColor>(BrightColor::Yellow);
                break;
            case 104:
                emit<SetBackgroundColor>(BrightColor::Blue);
                break;
            case 105:
                emit<SetBackgroundColor>(BrightColor::Magenta);
                break;
            case 106:
                emit<SetBackgroundColor>(BrightColor::Cyan);
                break;
            case 107:
                emit<SetBackgroundColor>(BrightColor::White);
                break;
            default:
                logUnsupportedCSI();
                break;
        }
    }
}

template <typename T>
size_t OutputHandler::parseColor(size_t i)
{
    if (i + 1 < parameterCount())
    {
        ++i;
        auto const mode = param(i);
        if (mode == 5)
        {
            if (i + 1 < parameterCount())
            {
                ++i;
                auto const value = param(i);
                if (i <= 255)
                    emit<T>(static_cast<IndexedColor>(value));
                else
                    logInvalidCSI("Invalid color indexing.");
            }
            else
                logInvalidCSI("Missing color index.");
        }
        else if (mode == 2)
        {
            if (i + 3 < parameterCount())
            {
                auto const r = param(i + 1);
				auto const g = param(i + 2);
				auto const b = param(i + 3);
                i += 3;
                if (r <= 255 && g <= 255 && b <= 255)
                    emit<T>(RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
                else
                    logInvalidCSI("RGB color out of range.");
            }
            else
                logInvalidCSI("Invalid color mode.");
        }
        else
            logInvalidCSI("Invalid color mode.");
        // TODO: add extension for RGB hex coded colors (6;R;G;Bm), with
        // R, G, and B being 8-bit hexadecimal values, for example FF for 255.
    }
    else
        logInvalidCSI("Invalid color indexing.");

    return i;
}

void OutputHandler::logUnsupported(std::string_view const& msg) const
{
    log<UnsupportedOutputEvent>(msg);
}

void OutputHandler::logUnsupportedCSI() const
{
    log<UnsupportedOutputEvent>(sequenceString("CSI"));
}

void OutputHandler::logInvalidESC(std::string const& message) const
{
	if (message.empty())
    	log<InvalidOutputEvent>("{}.", sequenceString("ESC"));
	else
    	log<InvalidOutputEvent>("{} ({})", sequenceString("ESC"), message);
}

void OutputHandler::logInvalidCSI(std::string const& message) const
{
	if (message.empty())
		log<InvalidOutputEvent>("{}", sequenceString("CSI"));
	else
		log<InvalidOutputEvent>("{} ({})", sequenceString("CSI"), message);
}

}  // namespace terminal
