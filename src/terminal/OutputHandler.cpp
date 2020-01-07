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

OutputHandler::OutputHandler(Logger _logger) :
	logger_{std::move(_logger)},
	functionMapper_{functions(VTType::VT525)}
{
	parameters_.reserve(MaxParameters);
}

string OutputHandler::sequenceString(char _finalChar, string const& _prefix) const
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

    sstr << ' ' << _finalChar;

    return sstr.str();
}

void OutputHandler::invokeAction(ActionClass /*_actionClass*/, Action _action, char32_t _currentChar)
{
    currentChar_ = _currentChar;

    switch (_action)
    {
        case Action::Clear:
			leaderSymbol_ = 0;
            intermediateCharacters_.clear();
            parameters_.resize(1);
            parameters_[0] = 0;
            private_ = false;
            return;
		case Action::CollectLeader:
			leaderSymbol_ = static_cast<char>(_currentChar);
			return;
        case Action::Collect:
            intermediateCharacters_.push_back(static_cast<char>(_currentChar)); // cast OK, because non-ASCII wouldn't be valid collected chars
            return;
        case Action::Print:
            emitCommand<AppendChar>(_currentChar);
            return;
        case Action::Param:
            if (_currentChar == ';')
                parameters_.push_back(0);
            else
                parameters_.back() = parameters_.back() * 10 + (_currentChar - U'0');
            return;
        case Action::CSI_Dispatch:
			dispatchCSI(static_cast<char>(_currentChar));
            return;
        case Action::Execute:
            executeControlFunction(static_cast<char>(_currentChar));
            return;
        case Action::ESC_Dispatch:
            if (intermediateCharacters_.empty())
                dispatchESC(static_cast<char>(_currentChar));
            else if (intermediateCharacters_ == "#" && _currentChar == '8')
                emitCommand<ScreenAlignmentPattern>();
            else if (intermediateCharacters_ == "(" && _currentChar == 'B')
            {
                // TODO: ESC ( B
                log<UnsupportedOutputEvent>("Designate G0 Character Set: US-ASCII.");
            }
            else if (_currentChar == '0')
            {
                if (auto g = getCharsetTableForCode(intermediateCharacters_); g.has_value())
                    emitCommand<DesignateCharset>(*g, Charset::Special);
                else
                    logInvalidESC(static_cast<char>(_currentChar), fmt::format("Invalid charset table identifier: {}", escape(intermediateCharacters_[0])));
            }
            else
                logInvalidESC(static_cast<char>(_currentChar));
            return;
        case Action::OSC_Start:
            // no need, we inline OSC_Put and OSC_End's actions
            break;
        case Action::OSC_Put:
            intermediateCharacters_.push_back(static_cast<char>(_currentChar)); // cast OK, becuase only ASCII's allowed (I think) TODO: check that fact
            break;
        case Action::OSC_End:
            dispatchOSC();
            intermediateCharacters_.clear();
            break;
        case Action::Hook:
        case Action::Put:
        case Action::Unhook:
            log<UnsupportedOutputEvent>(fmt::format(
                "Action: {} {} \"{}\"", to_string(_action), escape(_currentChar),
                escape(intermediateCharacters_)));
            return;
        case Action::Ignore:
        case Action::Undefined:
            return;
    }
}

void OutputHandler::dispatchOSC()
{
    auto const [code, value] = [](std::string const& _data) {
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

        string const value = _data.substr(i);
        return pair{code, value};
    }(intermediateCharacters_);

    switch (code)
    {
        case 0: // set window title and icon name
        case 2: // set window title
            emitCommand<ChangeWindowTitle>(value);
            break;
        case 1: // set icon name
            // ignore
            break;
        case 3: // set X server property
        case 4: // Ps = 4 ; c ; spec -> Change Color Number c to the color specified by spec.
        case 5: // Ps = 5 ; c ; spec -> Change Special Color Number c to the color specified by spec.
        case 6: // Ps = 6 ; c ; f -> Enable/disable Special Color Number c.
        case 10: // Ps = 1 0  -> Change VT100 text foreground color to Pt.
        case 11: // Ps = 1 1  -> Change VT100 text background color to Pt.
        case 12: // Ps = 1 2  -> Change text cursor color to Pt.
        case 13: // Ps = 1 3  -> Change mouse foreground color to Pt.
        case 14: // Ps = 1 4  -> Change mouse background color to Pt.
        case 15: // Ps = 1 5  -> Change Tektronix foreground color to Pt.
        case 16: // Ps = 1 6  -> Change Tektronix background color to Pt.
        case 17: // Ps = 1 7  -> Change highlight background color to Pt.
        case 18: // Ps = 1 8  -> Change Tektronix cursor color to Pt.
        case 19: // Ps = 1 9  -> Change highlight foreground color to Pt.
        case 46: // Ps = 4 6  -> Change Log File to Pt.  This is normally disabled by a compile-time option.
        case 50: // Ps = 5 0  -> Set Font to Pt.
        case 51: // Ps = 5 1  -> reserved for Emacs shell.
        case 52: // Ps = 5 2  -> Manipulate Selection Data.
        case 104: // Ps = 1 0 4 ; c -> Reset Color Number c.
        case 105: // Ps = 1 0 5 ; c -> Reset Special Color Number c.
        case 106: // Ps = 1 0 6 ; c ; f -> Enable/disable Special Color Number c.
        case 110: // Ps = 1 1 0  -> Reset VT100 text foreground color.
        case 111: // Ps = 1 1 1  -> Reset VT100 text background color.
        case 112: // Ps = 1 1 2  -> Reset text cursor color.
        case 113: // Ps = 1 1 3  -> Reset mouse foreground color.
        case 114: // Ps = 1 1 4  -> Reset mouse background color.
        case 115: // Ps = 1 1 5  -> Reset Tektronix foreground color.
        case 116: // Ps = 1 1 6  -> Reset Tektronix background color.
        case 117: // Ps = 1 1 7  -> Reset highlight color.
        case 118: // Ps = 1 1 8  -> Reset Tektronix cursor color.
        case 119: // Ps = 1 1 9  -> Reset highlight foreground color.
        case -'I': // Ps = I  ; c -> Set icon to file.
        case -'l': // Ps = l  ; c -> Set window title.
        case -'L': // Ps = L  ; c -> Set icon label.
            log<UnsupportedOutputEvent>("OSC " + intermediateCharacters_);
            break;
        default:
            log<InvalidOutputEvent>("OSC " + intermediateCharacters_, "Unknown");
            break;
    }
}

void OutputHandler::executeControlFunction(char _c0)
{
    switch (_c0)
    {
        case 0x07: // BEL
            emitCommand<Bell>();
            break;
        case 0x08: // BS
            emitCommand<Backspace>();
            break;
        case 0x09: // TAB
            emitCommand<MoveCursorToNextTab>();
            break;
        case 0x0A: // LF
            emitCommand<Linefeed>();
            break;
        case 0x0B: // VT
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            emitCommand<Index>();
            break;
        case 0x0C: // FF
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            emitCommand<Index>();
            break;
        case 0x0D:
            emitCommand<MoveCursorToBeginOfLine>();
            break;
        case 0x37:
            emitCommand<SaveCursor>();
            break;
        case 0x38:
            emitCommand<RestoreCursor>();
            break;
        default:
            log<UnsupportedOutputEvent>(escape(_c0));
            break;
    }
}

void OutputHandler::dispatchESC(char _finalChar)
{
    char const leaderSym = intermediateCharacters_.size() == 1
		? intermediateCharacters_[0]
		: 0;

	auto const funcId = FunctionDef::makeId(
		FunctionType::ESC, leaderSym, 0, static_cast<char>(_finalChar));

	if (auto const funcMap = functionMapper_.find(funcId); funcMap != end(functionMapper_))
		funcMap->second.second(*this);
	else
		logInvalidESC(_finalChar, "Unknown final character");
}

void OutputHandler::dispatchCSI(char _finalChar)
{
    char const followerSym = intermediateCharacters_.size() == 1
		? intermediateCharacters_[0]
		: 0;

	auto const funcId = FunctionDef::makeId(FunctionType::CSI, leaderSymbol_, followerSym, static_cast<char>(_finalChar));

	if (auto const funcMap = functionMapper_.find(funcId); funcMap != end(functionMapper_))
	{
		auto const result = funcMap->second.second(*this);
		switch (result)
		{
			case HandlerResult::Unsupported:
				logUnsupportedCSI(_finalChar);
				break;
			case HandlerResult::Invalid:
				logInvalidCSI(_finalChar);
				break;
			case HandlerResult::Ok:
				break;
		}
	}
}

void OutputHandler::logUnsupportedCSI(char _finalChar) const
{
    log<UnsupportedOutputEvent>(sequenceString(_finalChar, "CSI"));
}

void OutputHandler::logInvalidESC(char _finalChar, std::string const& message) const
{
    log<InvalidOutputEvent>(sequenceString(_finalChar, "ESC"), message);
}

void OutputHandler::logInvalidCSI(char _finalChar, std::string const& message) const
{
    log<InvalidOutputEvent>(sequenceString(_finalChar, "CSI"), message);
}

}  // namespace terminal
