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
#include <terminal/OutputHandler.h>
#include <terminal/FunctionDef.h>

#include <terminal/Commands.h>
#include <crispy/escape.h>
#include <crispy/utils.h>
#include <unicode/utf8.h>

#include <fmt/format.h>

#include <numeric>
#include <optional>
#include <sstream>

using namespace std;
using namespace crispy;

namespace terminal {

inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(std::string_view const& s)
{
    return crispy::splitKeyValuePairs(s, ':');
}

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

    if (parameters_.size() > 1 || (parameters_.size() == 1 && parameters_[0][0] != 0))
    sstr << ' ' << accumulate(
        begin(parameters_), end(parameters_), string{},
        [](string const& a, auto const& p) -> string {
            return !a.empty()
                ? fmt::format("{};{}",
                        a,
                        accumulate(
                            begin(p), end(p),
                            string{},
                            [](string const& x, FunctionParam y) -> string {
                                return !x.empty()
                                    ? fmt::format("{}:{}", x, y)
                                    : std::to_string(y);
                            }
                        )
                    )
                : accumulate(
                        begin(p), end(p),
                        string{},
                        [](string const& x, FunctionParam y) -> string {
                            return !x.empty()
                                ? fmt::format("{}:{}", x, y)
                                : std::to_string(y);
                        }
                    );
        }
    );

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
            parameters_[0] = {0};
            private_ = false;
            return;
		case Action::CollectLeader:
			leaderSymbol_ = static_cast<char>(_currentChar);
			return;
        case Action::Collect:
            {
                uint8_t u8[4];
                size_t const count = unicode::to_utf8(_currentChar, u8);
                for (size_t i = 0; i < count; ++i)
                    intermediateCharacters_.push_back(u8[i]);
            }
            return;
        case Action::Print:
            emitCommand<AppendChar>(_currentChar);
            return;
        case Action::Param:
            if (_currentChar == ';')
                parameters_.push_back({0});
            else if (_currentChar == ':')
                parameters_.back().push_back({0});
            else
                parameters_.back().back() = parameters_.back().back() * 10 + (_currentChar - U'0');
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
                //log<UnsupportedOutputEvent>("Designate G0 Character Set: US-ASCII.");
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
            {
                uint8_t u8[4];
                size_t const count = unicode::to_utf8(_currentChar, u8);
                for (size_t i = 0; i < count; ++i)
                    intermediateCharacters_.push_back(u8[i]);
            }
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

constexpr unsigned long strntoul(char const* _data, size_t _count, char const** _eptr, unsigned _base = 10)
{
    constexpr auto values = string_view{"0123456789ABCDEF"};
    constexpr auto lowerLetters = string_view{"abcdef"};

    unsigned long result = 0;
    while (_count != 0)
    {
        if (auto const i = values.find(*_data); i != values.npos && i < _base)
        {
            result *= _base;
            result += i;
            ++_data;
            --_count;
        }
        else if (auto const i = lowerLetters.find(*_data); i != lowerLetters.npos && _base == 16)
        {
            result *= _base;
            result += i;
            ++_data;
            --_count;
        }
        else
            return 0;
    }

    if (_eptr)
        *_eptr = _data;

    return result;
}

std::optional<RGBColor> OutputHandler::parseColor(std::string_view const& _value)
{
    try
    {
        // "rgb:RRRR/GGGG/BBBB"
        if (_value.size() == 18 && _value.substr(0, 4) == "rgb:" && _value[8] == '/' && _value[13] == '/')
        {
            auto const r = strntoul(_value.data() + 4, 4, nullptr, 16);
            auto const g = strntoul(_value.data() + 9, 4, nullptr, 16);
            auto const b = strntoul(_value.data() + 14, 4, nullptr, 16);

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

        return pair{code, string_view(_data.data() + i, _data.size() - i)};
    }(intermediateCharacters_);

    switch (code)
    {
        case 0: // set window title and icon name
        case 2: // set window title
            emitCommand<ChangeWindowTitle>(string(value));
            [[fallthrough]];
        case 1: // set icon name
            // ignore
            break;
        case 3: // set X server property
        case 4: // Ps = 4 ; c ; spec -> Change Color Number c to the color specified by spec.
        case 5: // Ps = 5 ; c ; spec -> Change Special Color Number c to the color specified by spec.
        case 6: // Ps = 6 ; c ; f -> Enable/disable Special Color Number c.
            log<UnsupportedOutputEvent>("OSC " + intermediateCharacters_);
            break;
        case 8: // hyperlink extension: https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
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
                emitCommand<Hyperlink>(string(id), string(uri));
            }
            else
                log<UnsupportedOutputEvent>("OSC " + intermediateCharacters_);
            break;
        case 10: // Ps = 1 0  -> Change VT100 text foreground color to Pt.
            if (value == "?")
                emitCommand<RequestDynamicColor>(DynamicColorName::DefaultForegroundColor);
            else if (auto color = parseColor(value); color.has_value())
                emitCommand<SetDynamicColor>(DynamicColorName::DefaultForegroundColor, color.value());
            else
                log<InvalidOutputEvent>("OSC {}", intermediateCharacters_);
            break;
        case 11: // Ps = 1 1  -> Change VT100 text background color to Pt.
            if (value == "?")
                emitCommand<RequestDynamicColor>(DynamicColorName::DefaultBackgroundColor);
            else if (auto color = parseColor(value); color.has_value())
                emitCommand<SetDynamicColor>(DynamicColorName::DefaultBackgroundColor, color.value());
            else
                log<InvalidOutputEvent>("OSC {}", intermediateCharacters_);
            break;
        case 12: // Ps = 1 2  -> Change text cursor color to Pt.
            if (value == "?")
                emitCommand<RequestDynamicColor>(DynamicColorName::TextCursorColor);
            else if (auto color = parseColor(value); color.has_value())
                emitCommand<SetDynamicColor>(DynamicColorName::TextCursorColor, color.value());
            else
                log<InvalidOutputEvent>("OSC {}", intermediateCharacters_);
            return;
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
            log<UnsupportedOutputEvent>("OSC " + intermediateCharacters_);
            break;
        case 110: // Ps = 1 1 0  -> Reset VT100 text foreground color.
            emitCommand<ResetDynamicColor>(DynamicColorName::DefaultForegroundColor);
            break;
        case 111: // Ps = 1 1 1  -> Reset VT100 text background color.
            emitCommand<ResetDynamicColor>(DynamicColorName::DefaultBackgroundColor);
            break;
        case 112: // Ps = 1 1 2  -> Reset text cursor color.
            emitCommand<ResetDynamicColor>(DynamicColorName::TextCursorColor);
            break;
        case 113: // Ps = 1 1 3  -> Reset mouse foreground color.
            emitCommand<ResetDynamicColor>(DynamicColorName::MouseForegroundColor);
            break;
        case 114: // Ps = 1 1 4  -> Reset mouse background color.
            emitCommand<ResetDynamicColor>(DynamicColorName::MouseBackgroundColor);
            break;
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
        case 777:
            {
                auto const splits = crispy::split(value, ';');
                if (splits.size() == 3 && splits[0] == "notify")
                    emitCommand<Notify>(string(splits[1]), string(splits[2]));
            }
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
            [[fallthrough]];
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
		? static_cast<char>(intermediateCharacters_[0])
		: char{};

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
		? static_cast<char>(intermediateCharacters_[0])
		: char{};

	auto const funcId = FunctionDef::makeId(FunctionType::CSI, leaderSymbol_, followerSym, static_cast<char>(_finalChar));

	if (auto const funcMap = functionMapper_.find(funcId); funcMap != end(functionMapper_))
	{
		HandlerResult const result = funcMap->second.second(*this);
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
