// SPDX-License-Identifier: Apache-2.0
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

namespace vtbackend
{

Sequencer::Sequencer(Terminal& terminal): _terminal { terminal }, _parameterBuilder { _sequence.parameters() }
{
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void Sequencer::error(std::string_view errorString)
{
    if (vtParserLog)
        vtParserLog()("Parser error: {}", errorString);
}

void Sequencer::print(char32_t codepoint)
{
    _terminal.state().instructionCounter++;
    _terminal.sequenceHandler().writeText(codepoint);
}

size_t Sequencer::print(string_view chars, size_t cellCount)
{
    assert(!chars.empty());

    _terminal.state().instructionCounter += chars.size();
    _terminal.sequenceHandler().writeText(chars, cellCount);

    return _terminal.settings().pageSize.columns.as<size_t>()
           - _terminal.currentScreen().cursor().position.column.as<size_t>();
}

void Sequencer::printEnd()
{
    _terminal.sequenceHandler().writeTextEnd();
}

void Sequencer::execute(char controlCode)
{
    _terminal.sequenceHandler().executeControlCode(controlCode);
}

void Sequencer::collect(char ch)
{
    _sequence.intermediateCharacters().push_back(ch);
}

void Sequencer::collectLeader(char leader) noexcept
{
    _sequence.setLeader(leader);
}

void Sequencer::param(char ch) noexcept
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

void Sequencer::dispatchESC(char finalChar)
{
    _sequence.setCategory(FunctionCategory::ESC);
    _sequence.setFinalChar(finalChar);
    handleSequence();
}

void Sequencer::dispatchCSI(char finalChar)
{
    _sequence.setCategory(FunctionCategory::CSI);
    _sequence.setFinalChar(finalChar);
    handleSequence();
}

void Sequencer::startOSC()
{
    _sequence.setCategory(FunctionCategory::OSC);
}

void Sequencer::putOSC(char ch)
{
    if (_sequence.intermediateCharacters().size() + 1 < Sequence::MaxOscLength)
        _sequence.intermediateCharacters().push_back(ch);
}

void Sequencer::dispatchOSC()
{
    auto const [code, skipCount] = vtparser::extractCodePrefix(_sequence.intermediateCharacters());
    _parameterBuilder.set(static_cast<Sequence::Parameter>(code));
    _sequence.intermediateCharacters().erase(0, skipCount);
    handleSequence();
    clear();
}

void Sequencer::hook(char finalChar)
{
    _terminal.state().instructionCounter++;
    _sequence.setCategory(FunctionCategory::DCS);
    _sequence.setFinalChar(finalChar);

    handleSequence();
}

void Sequencer::put(char ch)
{
    if (_hookedParser)
        _hookedParser->pass(ch);
}

void Sequencer::unhook()
{
    if (_hookedParser)
    {
        _hookedParser->finalize();
        _hookedParser.reset();
    }
}

size_t Sequencer::maxBulkTextSequenceWidth() const noexcept
{
    if (!_terminal.isPrimaryScreen())
        return 0;

    if (!_terminal.primaryScreen().currentLine().isTrivialBuffer())
        return 0;

    assert(_terminal.state().margin.horizontal.to >= _terminal.currentScreen().cursor().position.column);
    return unbox<size_t>(_terminal.state().margin.horizontal.to
                         - _terminal.currentScreen().cursor().position.column);
}

void Sequencer::handleSequence()
{
    _parameterBuilder.fixiate();
    _terminal.sequenceHandler().processSequence(_sequence);
}

} // namespace vtbackend
