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

void OutputHandler::invokeAction(ActionClass _actionClass, Action _action, char32_t _currentChar)
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
            emit<AppendChar>(_currentChar);
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
                emit<ScreenAlignmentPattern>();
            else if (intermediateCharacters_ == "(" && _currentChar == 'B')
                logUnsupported("Designate Character Set US-ASCII.");
            else if (_currentChar == '0')
            {
                if (auto g = getCharsetTableForCode(intermediateCharacters_); g.has_value())
                    emit<DesignateCharset>(*g, Charset::Special);
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
                        log<UnsupportedOutputEvent>("OSC " + intermediateCharacters_);
                        break;
                    default:
                    {
                        log<InvalidOutputEvent>("OSC " + intermediateCharacters_);
                        break;
                    }
                }
            }
            else
            {
                log<UnsupportedOutputEvent>("OSC " + intermediateCharacters_);
            }
            intermediateCharacters_.clear();
            break;
        case Action::Hook:
        case Action::Put:
        case Action::Unhook:
            logUnsupported("Action: {} {} \"{}\"", to_string(_action), escape(_currentChar),
                           escape(intermediateCharacters_));
            return;
        case Action::Ignore:
        case Action::Undefined:
            return;
    }
}

void OutputHandler::executeControlFunction(char _c0)
{
    switch (_c0)
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
            logUnsupported("ESC C0 or C1 control function: {}", escape(_c0));
            break;
    }
}

void OutputHandler::dispatchESC(char _finalChar)
{
    switch (_finalChar)
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
            logUnsupported("ESC_Dispatch: '{}' {}", escape(_finalChar), escape(intermediateCharacters_));
            break;
    }
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

void OutputHandler::logUnsupported(std::string_view const& msg) const
{
    log<UnsupportedOutputEvent>(msg);
}

void OutputHandler::logUnsupportedCSI(char _finalChar) const
{
    log<UnsupportedOutputEvent>(sequenceString(_finalChar, "CSI"));
}

void OutputHandler::logInvalidESC(char _finalChar, std::string const& message) const
{
	if (message.empty())
    	log<InvalidOutputEvent>("{}.", sequenceString(_finalChar, "ESC"));
	else
    	log<InvalidOutputEvent>("{} ({})", sequenceString(_finalChar, "ESC"), message);
}

void OutputHandler::logInvalidCSI(char _finalChar, std::string const& message) const
{
	if (message.empty())
		log<InvalidOutputEvent>("{}", sequenceString(_finalChar, "CSI"));
	else
		log<InvalidOutputEvent>("{} ({})", sequenceString(_finalChar, "CSI"), message);
}

}  // namespace terminal
