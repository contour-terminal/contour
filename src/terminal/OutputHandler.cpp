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
	logger_{std::move(_logger)}
{
}

void OutputHandler::invokeAction(ActionClass /*_actionClass*/, Action _action, char32_t _currentChar)
{
    switch (_action)
    {
        case Action::Clear:
            sequence_.clear();
            return;
		case Action::CollectLeader:
			sequence_.setLeader(static_cast<char>(_currentChar));
			return;
        case Action::Collect:
            {
                uint8_t u8[4];
                size_t const count = unicode::to_utf8(_currentChar, u8);
                for (size_t i = 0; i < count; ++i)
                    sequence_.intermediateCharacters().push_back(u8[i]);
            }
            return;
        case Action::Print:
            emitCommand<AppendChar>(_currentChar);
            return;
        case Action::Param:
            if (sequence_.parameters().empty())
                sequence_.parameters().push_back({0});
            if (_currentChar == ';')
                sequence_.parameters().push_back({0});
            else if (_currentChar == ':')
                sequence_.parameters().back().push_back({0});
            else
                sequence_.parameters().back().back() = sequence_.parameters().back().back() * 10 + (_currentChar - U'0');
            return;
        case Action::CSI_Dispatch:
			dispatchCSI(static_cast<char>(_currentChar));
            return;
        case Action::Execute:
            executeControlFunction(static_cast<char>(_currentChar));
            return;
        case Action::ESC_Dispatch:
            dispatchESC(static_cast<char>(_currentChar));
            return;
        case Action::OSC_Start:
            // no need, we inline OSC_Put and OSC_End's actions
            break;
        case Action::OSC_Put:
            {
                uint8_t u8[4];
                size_t const count = unicode::to_utf8(_currentChar, u8);
                for (size_t i = 0; i < count; ++i)
                    sequence_.intermediateCharacters().push_back(u8[i]);
            }
            break;
        case Action::OSC_End:
            dispatchOSC();
            sequence_.clear();
            break;
        case Action::Hook:
        case Action::Put:
        case Action::Unhook:
            log<UnsupportedOutputEvent>(fmt::format(
                "Action: {} {} \"{}\"", to_string(_action), escape(unicode::to_utf8(_currentChar)),
                escape(sequence_.intermediateCharacters())));
            return;
        case Action::Ignore:
        case Action::Undefined:
            return;
    }
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
    }(sequence_.intermediateCharacters());

    sequence_.setCategory(FunctionCategory::OSC);
    sequence_.parameters().push_back({static_cast<Sequence::Parameter>(code)});
    sequence_.intermediateCharacters() = string{value};

    if (FunctionSpec const* funcSpec = select(sequence_.selector()); funcSpec != nullptr)
        apply(*funcSpec, sequence_, commands_);
    else
        log<InvalidOutputEvent>(sequence_.str(), "Sequencer: Unsupported OSC function.");
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
    sequence_.setCategory(FunctionCategory::ESC);
    sequence_.setFinalChar(_finalChar);

    if (FunctionSpec const* funcSpec = select(sequence_.selector()); funcSpec != nullptr)
        apply(*funcSpec, sequence_, commands_);
    else
		logInvalidESC("Unknown escape sequence.");
}

void OutputHandler::dispatchCSI(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::CSI);
    sequence_.setFinalChar(_finalChar);

    if (FunctionSpec const* funcSpec = select(sequence_.selector()); funcSpec != nullptr)
	{
        ApplyResult const result = apply(*funcSpec, sequence_, commands_);
		switch (result)
		{
            case ApplyResult::Unsupported:
				logUnsupportedCSI();
				break;
            case ApplyResult::Invalid:
				logInvalidCSI();
				break;
            case ApplyResult::Ok:
				break;
		}
	}
    else
        logInvalidCSI();
}

void OutputHandler::logUnsupportedCSI() const
{
    log<UnsupportedOutputEvent>(sequence_.str());
}

void OutputHandler::logInvalidESC(std::string const& message) const
{
    log<InvalidOutputEvent>(sequence_.str(), message);
}

void OutputHandler::logInvalidCSI(std::string const& message) const
{
    log<InvalidOutputEvent>(sequence_.str(), message);
}

}  // namespace terminal
