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
#include <vtbackend/Screen.h>
#include <vtbackend/Sequencer.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <string_view>
#include <utility>

using std::get;
using std::holds_alternative;
using std::string_view;

using namespace std::string_view_literals;

namespace terminal
{

sequencer::sequencer(Terminal& terminal):
    _terminal { terminal }, _parameterBuilder { _sequence.getParameters() }
{
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void sequencer::error(std::string_view errorString)
{
    if (VTParserLog)
        VTParserLog()("Parser error: {}", errorString);
}

void sequencer::print(char32_t codepoint)
{
    _terminal.state().instructionCounter++;
    _terminal.sequenceHandler().writeText(codepoint);
}

size_t sequencer::print(string_view chars, size_t cellCount)
{
    assert(!chars.empty());

    _terminal.state().instructionCounter += chars.size();
    _terminal.sequenceHandler().writeText(chars, cellCount);

    return _terminal.getSettings().pageSize.columns.as<size_t>()
           - _terminal.currentScreen().getCursor().position.column.as<size_t>();
}

void sequencer::execute(char controlCode)
{
    _terminal.sequenceHandler().executeControlCode(controlCode);
}

void sequencer::collect(char ch)
{
    _sequence.intermediateCharacters().push_back(ch);
}

void sequencer::collectLeader(char leader) noexcept
{
    _sequence.setLeader(leader);
}

void sequencer::param(char ch) noexcept
{
    switch (ch)
    {
        case ';': paramSeparator(); break;
        case ':': paramSubSeparator(); break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': paramDigit(ch); break;
    }
}

void sequencer::dispatchESC(char finalChar)
{
    _sequence.setCategory(function_category::ESC);
    _sequence.setFinalChar(finalChar);
    handleSequence();
}

void sequencer::dispatchCSI(char finalChar)
{
    _sequence.setCategory(function_category::CSI);
    _sequence.setFinalChar(finalChar);
    handleSequence();
}

void sequencer::startOSC()
{
    _sequence.setCategory(function_category::OSC);
}

void sequencer::putOSC(char ch)
{
    if (_sequence.intermediateCharacters().size() + 1 < sequence::MaxOscLength)
        _sequence.intermediateCharacters().push_back(ch);
}

void sequencer::dispatchOSC()
{
    auto const [code, skipCount] = parser::extractCodePrefix(_sequence.intermediateCharacters());
    _parameterBuilder.set(static_cast<sequence::parameter>(code));
    _sequence.intermediateCharacters().erase(0, skipCount);
    handleSequence();
    clear();
}

void sequencer::hook(char finalChar)
{
    _terminal.state().instructionCounter++;
    _sequence.setCategory(function_category::DCS);
    _sequence.setFinalChar(finalChar);

    handleSequence();
}

void sequencer::put(char ch)
{
    if (_hookedParser)
        _hookedParser->pass(ch);
}

void sequencer::unhook()
{
    if (_hookedParser)
    {
        _hookedParser->finalize();
        _hookedParser.reset();
    }
}

size_t sequencer::maxBulkTextSequenceWidth() const noexcept
{
    if (!_terminal.isPrimaryScreen())
        return 0;

    if (!_terminal.primaryScreen().currentLine().isTrivialBuffer())
        return 0;

    assert(_terminal.currentScreen().margin().horizontal.to
           >= _terminal.currentScreen().cursor().position.column);
    return unbox<size_t>(_terminal.currentScreen().getMargin().hori.to
                         - _terminal.currentScreen().getCursor().position.column);
}

void sequencer::handleSequence()
{
    _parameterBuilder.fixiate();
    _terminal.sequenceHandler().processSequence(_sequence);
}

} // namespace terminal
